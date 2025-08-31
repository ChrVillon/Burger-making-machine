#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>
#include "burger_machine.h"
#include <sys/select.h>
#include <errno.h>

static BurgerSystem system_local;
static SharedControl *shared_ctrl = NULL;
static int ctrl_shmid = -1;

static pthread_t *band_threads; // Hilos para bandas
static pthread_t gen_thread; // Hilo para generador de órdenes
static pthread_t mon_thread; // Hilo para monitor de estado

// Funcion para verificar si hay ingredientes suficientes
static int checkIngredients(const Ingredients *a, const Ingredients *r) {
    return a->buns>=r->buns && a->tomato>=r->tomato && a->onion>=r->onion &&
           a->lettuce>=r->lettuce && a->meat>=r->meat && a->cheese>=r->cheese;
}

// Función para consumir ingredientes
static void useIngredients(Ingredients *a, const Ingredients *r) {
    a->buns -= r->buns; a->tomato -= r->tomato; a->onion -= r->onion;
    a->lettuce -= r->lettuce; a->meat -= r->meat; a->cheese -= r->cheese;
}

// Función para reponer ingredientes en una banda
static int restockBand(BurgerSystem *s, int i) {
    PreparationBand *b = &s->bands[i];
    int was_waiting = (b->state == WAITING_FOR_INGREDIENTS);
    b->available.buns    += 10;
    b->available.tomato  += 5;
    b->available.onion   += 5;
    b->available.lettuce += 5;
    b->available.meat    += 10;
    b->available.cheese  += 5;
    if (was_waiting) b->state = ACTIVE;
    return was_waiting;
}

// Inicializa el sistema
static void initSystem(BurgerSystem *s, int bands, Ingredients tpl) {
    memset(s,0,sizeof(*s));
    s->band_count = bands;
    s->order_template = tpl;
    s->running = 1;
    sem_init(&s->mutex, 0, 1);
    sem_init(&s->orders_sem, 0, 0);
    srand(time(NULL));
    for (int i = 0; i < bands; i++) {
        s->bands[i].id = i;
        s->bands[i].state = ACTIVE;
        s->bands[i].available.buns = 5;
        s->bands[i].available.tomato = 5;
        s->bands[i].available.onion = 5;
        s->bands[i].available.lettuce = 5;
        s->bands[i].available.meat = 5;
        s->bands[i].available.cheese = 5;
        s->bands[i].current_order = -1; // ninguna orden asignada
    }
}

// Crea y conecta el segmento de memoria compartida para control externo
static void createSharedControl(int bands) {
    size_t sz = sizeof(SharedControl);
    ctrl_shmid = shmget(CTRL_SHM_KEY, sz, IPC_CREAT | IPC_EXCL | 0666); // intenta crear memria compartida
    if (ctrl_shmid == -1) { 
        if (errno == EEXIST) { // Si ya existe
            // Intentar usar el existente
            ctrl_shmid = shmget(CTRL_SHM_KEY, sz, 0666);
            if (ctrl_shmid == -1) {
                if (errno == EINVAL) {
                    // Tamaño incompatible: eliminar y recrear
                    int old = shmget(CTRL_SHM_KEY, 0, 0666);
                    if (old != -1) shmctl(old, IPC_RMID, NULL);
                    ctrl_shmid = shmget(CTRL_SHM_KEY, sz, IPC_CREAT | 0666);
                }
            }
        }
    }

    if (ctrl_shmid == -1) { 
        perror("shmget control"); 
        exit(1); }

    shared_ctrl = (SharedControl*)shmat(ctrl_shmid, NULL, 0); // Conecta la memoria compartida
    if (shared_ctrl == (void*)-1) { 
        perror("shmat control"); 
        exit(1); 
    }
    
    // Inicializar los campos de SharedControl
    // Esta seccion ya es parte de la memoria compartida
    memset(shared_ctrl, 0, sizeof(*shared_ctrl));
    shared_ctrl->running = 1;
    shared_ctrl->band_count = bands;
    for (int i = 0; i < bands; i++) shared_ctrl->band_states[i] = ACTIVE;
    shared_ctrl->restock_request = 0;
    shared_ctrl->server_pid = getpid();
    sem_init(&shared_ctrl->mutex, 1, 1);
}

// Hilo para cada banda de preparación
static void* bandThread(void *arg) {
    int id = (int)(intptr_t)arg;
    BurgerSystem *s = &system_local;
    while (s->running && shared_ctrl->running) {
        sem_wait(&s->orders_sem);
        if (!s->running || !shared_ctrl->running) break;

        // Consultar estado externo (Si el control externo pausó esta banda)
        sem_wait(&shared_ctrl->mutex);
        BandState ext = shared_ctrl->band_states[id];
        sem_post(&shared_ctrl->mutex);
        if (ext == STOPPED) { // devolver orden a la cola
            sem_post(&s->orders_sem);
            usleep(150000); // sleep de 150ms para evitar busy loop
            continue;
        }

        sem_wait(&s->mutex);
        PreparationBand *b = &s->bands[id];
        Order *o = &s->orders[s->queue_front];

        // Verificar si hay ingredientes suficientes
        if (!checkIngredients(&b->available, &o->ingredients)) {
            // Marcar WAITING si no estaba
            if (b->state != WAITING_FOR_INGREDIENTS)
                b->state = WAITING_FOR_INGREDIENTS;

            // Liberar mutex del sistema
            sem_post(&s->mutex);

            // Devolver la orden a la cola
            sem_post(&s->orders_sem);

            // Pequeña espera antes de reintentar (evita busy loop)
            usleep(300000);
            continue;
        }
        // Tomar la orden y procesarla
        s->queue_front = (s->queue_front + 1) % MAX_ORDERS;
        s->queue_count--;
        b->current_order = o->id;
        b->state = PREPARING;
        useIngredients(&b->available, &o->ingredients);
        sem_post(&s->mutex);

        sleep(1 + rand()%2); // Simula tiempo de preparación (1-2s)

        // Marcar orden como completada
        sem_wait(&s->mutex);
        b->burgers_processed++;
        b->current_order = -1;
        b->state = ACTIVE;
        s->completed_orders++;
        sem_post(&s->mutex);
    }
    return NULL;
}

static void* orderGeneratorThread(void *arg) {
    (void)arg;
    BurgerSystem *s = &system_local;
    int id = 1;
    while (s->running && shared_ctrl->running) {
        sleep(1);
        // Procesar restock pedido externamente (control.c)
        sem_wait(&shared_ctrl->mutex);
        int need_restock = shared_ctrl->restock_request;
        if (need_restock) shared_ctrl->restock_request = 0;
        sem_post(&shared_ctrl->mutex);

        if (need_restock) {
            sem_wait(&s->mutex);
            for (int i = 0; i < s->band_count; i++)
                restockBand(s,i);  // Repone ingredientes en todas las bandas
            sem_post(&s->mutex);
        }

        sem_wait(&s->mutex);
        if (!s->running || !shared_ctrl->running) { 
            sem_post(&s->mutex); 
            break; 
        }

        // Generar nueva orden si hay espacio en la cola
        if (s->queue_count < MAX_ORDERS) {
            Order *o = &s->orders[s->queue_rear];
            o->id = id++;
            o->ingredients = s->order_template;
            s->queue_rear = (s->queue_rear + 1) % MAX_ORDERS;
            s->queue_count++;
            s->total_orders++;
            sem_post(&s->orders_sem);
        }
        sem_post(&s->mutex);
    }
    return NULL;
}

static void printTableHeader(void) {
    printf("\033[H\033[J"); // limpia pantalla
    printf("=============== BURGER MACHINE ===============\n");
    printf("Total:%d  Completadas:%d  Pendientes:%d/%d\n",
           system_local.total_orders,
           system_local.completed_orders,
           system_local.queue_count,
           MAX_ORDERS);
    printf("--------------------------------------------------------------------------\n");
    printf("Banda | Estado     | Proc | Orden | Pan Tom Ceb Lec Car Que | StockFlag\n");
    printf("--------------------------------------------------------------------------\n");
}

static void* monitorThread(void *arg) {
    (void)arg;
    BurgerSystem *s = &system_local;
    while (s->running && shared_ctrl->running) {
        sem_wait(&s->mutex);

        printTableHeader(); // Imprime encabezado

        for (int i = 0; i < s->band_count; i++) {
            sem_wait(&shared_ctrl->mutex);
            BandState ext = shared_ctrl->band_states[i]; // Estado externo (si fue pausada por control)
            sem_post(&shared_ctrl->mutex);

            PreparationBand *b = &s->bands[i];

            // Definir estado efectivo a mostrar
            const char *st;
            if (ext == STOPPED) st = "DETENIDO";
            else if (b->state == WAITING_FOR_INGREDIENTS) st = "EN ESPERA";
            else if (b->state == PREPARING) st = "PREPARANDO";
            else st = "ACTIVO";

            // Flag stock bajo (ejemplo si buns o meat < receta pedida)
            int low = (b->available.buns < s->order_template.buns ||
                       b->available.meat < s->order_template.meat);

            // Imprime estado de la banda
            printf(" %2d   | %-10s | %4d | %5d | %3d %3d %3d %3d %3d %3d | %s\n",
                   i + 1,
                   st,
                   b->burgers_processed,
                   b->current_order,
                   b->available.buns,
                   b->available.tomato,
                   b->available.onion,
                   b->available.lettuce,
                   b->available.meat,
                   b->available.cheese,
                   low ? "BAJO" : "-"); 

        }
        sem_post(&s->mutex);

        fflush(stdout); // Asegura que se imprima inmediatamente
        usleep(500000); // 0.5s
    }
    return NULL;
}

// Manejador de señal para cierre ordenado
static void handleSignal(int sig) {
    (void)sig;
    system_local.running = 0;
    if (shared_ctrl) shared_ctrl->running = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 8) {
        printf("Uso: %s <bandas> <pan> <tomate> <cebolla> <lechuga> <carne> <queso>\n", argv[0]);
        return 1;
    }
    int bands = atoi(argv[1]);
    if (bands <1 || bands > MAX_BANDS) { 
        printf("Bandas 1-%d\n", MAX_BANDS); 
        return 1; 
    }

    // Plantilla de ingredientes por orden
    Ingredients tpl = { atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                        atoi(argv[5]), atoi(argv[6]), atoi(argv[7]) };
    if (tpl.buns<1 || tpl.meat<1) { 
        printf("Pan y carne >=1\n"); 
        return 1; 
    }

    // Inicialización del sistema
    initSystem(&system_local, bands, tpl);
    createSharedControl(bands);

    // Configurar manejadores de señal para cierre ldel programa
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    band_threads = malloc(sizeof(pthread_t)*bands); // reservar memoria para hilos de bandas
    for (int i = 0; i < bands; i++)
        pthread_create(&band_threads[i], NULL, bandThread, (void*)(intptr_t)i);
    pthread_create(&gen_thread, NULL, orderGeneratorThread, NULL);
    pthread_create(&mon_thread, NULL, monitorThread, NULL);

    while (system_local.running && shared_ctrl->running) {
        // Usar select para esperar entrada en stdin con timeout si se quire controlar desde burger_machine
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200 ms

        int ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);

        // Revisa si control externo pidió parar mientras esperábamos
        if (!shared_ctrl->running) break;

        // Maneja entrada de usuario si hay
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            int c = getchar();
            if (c == EOF) break;
            if (c == '\n') continue;

            if (c >= '1' && c <= '9') {
                int id = c - '1';
                if (id < bands) {
                    sem_wait(&shared_ctrl->mutex);
                    shared_ctrl->band_states[id] =
                        (shared_ctrl->band_states[id]==STOPPED)?ACTIVE:STOPPED;
                    sem_post(&shared_ctrl->mutex);
                }
            } else if (c=='r' || c=='R') {
                sem_wait(&shared_ctrl->mutex);
                shared_ctrl->restock_request = 1;
                sem_post(&shared_ctrl->mutex);
            } else if (c=='q' || c=='Q') {
                shared_ctrl->running = 0;
                break;
            }
        }
    }

    // Señal de cierre interna cuando control externo o local sale
    system_local.running = 0;
    shared_ctrl->running = 0;

    // Despertar hilos bloqueados en orders_sem
    for (int i = 0; i < system_local.band_count; i++)
        sem_post(&system_local.orders_sem);

    for (int i = 0; i < bands; i++) pthread_join(band_threads[i], NULL);
    pthread_join(gen_thread, NULL);
    pthread_join(mon_thread, NULL);

    sem_destroy(&shared_ctrl->mutex);
    shmdt(shared_ctrl);

    sem_destroy(&system_local.mutex);
    sem_destroy(&system_local.orders_sem);
    free(band_threads);
    return 0;
}