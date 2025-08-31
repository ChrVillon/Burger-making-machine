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

static pthread_t *band_threads;
static pthread_t gen_thread;
static pthread_t mon_thread;

/* -------- Utilidades -------- */
static int checkIngredients(const Ingredients *a, const Ingredients *r) {
    return a->buns>=r->buns && a->tomato>=r->tomato && a->onion>=r->onion &&
           a->lettuce>=r->lettuce && a->meat>=r->meat && a->cheese>=r->cheese;
}
static void useIngredients(Ingredients *a, const Ingredients *r) {
    a->buns -= r->buns; a->tomato -= r->tomato; a->onion -= r->onion;
    a->lettuce -= r->lettuce; a->meat -= r->meat; a->cheese -= r->cheese;
}
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

/* -------- Inicialización -------- */
static void initSystem(BurgerSystem *s, int bands, Ingredients tpl) {
    memset(s,0,sizeof(*s));
    s->band_count = bands;
    s->order_template = tpl;
    s->running = 1;
    sem_init(&s->mutex, 0, 1);
    sem_init(&s->orders_sem, 0, 0);
    srand(time(NULL));
    for (int i=0;i<bands;i++) {
        s->bands[i].id = i;
        s->bands[i].state = ACTIVE;
        s->bands[i].available.buns=5;
        s->bands[i].available.tomato=5;
        s->bands[i].available.onion=5;
        s->bands[i].available.lettuce=5;
        s->bands[i].available.meat=5;
        s->bands[i].available.cheese=5;
        s->bands[i].current_order = -1;
    }
}
static void createSharedControl(int bands) {
    size_t sz = sizeof(SharedControl);
    ctrl_shmid = shmget(CTRL_SHM_KEY, sz, IPC_CREAT | IPC_EXCL | 0666);
    if (ctrl_shmid == -1) {
        if (errno == EEXIST) {
            // Intentar usar el existente; si tamaño distinto -> limpiar y recrear
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
    if (ctrl_shmid == -1) { perror("shmget control"); exit(1); }

    shared_ctrl = (SharedControl*)shmat(ctrl_shmid, NULL, 0);
    if (shared_ctrl == (void*)-1) { perror("shmat control"); exit(1); }

    // Si venimos de segmento recién creado (suponemos limpio) o forzamos init
    memset(shared_ctrl, 0, sizeof(*shared_ctrl));
    shared_ctrl->running = 1;
    shared_ctrl->band_count = bands;
    for (int i=0;i<bands;i++) shared_ctrl->band_states[i] = ACTIVE;
    shared_ctrl->restock_request = 0;
    shared_ctrl->server_pid = getpid();
    sem_init(&shared_ctrl->mutex, 1, 1);
}

/* -------- Hilos -------- */
static void* bandThread(void *arg) {
    int id = (int)(intptr_t)arg;
    BurgerSystem *s = &system_local;
    while (s->running && shared_ctrl->running) {
        if (sem_wait(&s->orders_sem) == -1) continue;
        if (!s->running || !shared_ctrl->running) break;

        // Consultar estado externo
        sem_wait(&shared_ctrl->mutex);
        BandState ext = shared_ctrl->band_states[id];
        sem_post(&shared_ctrl->mutex);
        if (ext == STOPPED) { // devolver crédito
            sem_post(&s->orders_sem);
            usleep(150000);
            continue;
        }

        sem_wait(&s->mutex);
        if (s->queue_count == 0) { sem_post(&s->mutex); continue; }
        PreparationBand *b = &s->bands[id];
        Order *o = &s->orders[s->queue_front];
        if (o->state != PENDING) {
            sem_post(&s->mutex);
            continue;
        }
        if (!checkIngredients(&b->available, &o->ingredients)) {
            // Marcar WAITING si no estaba
            if (b->state != WAITING_FOR_INGREDIENTS)
                b->state = WAITING_FOR_INGREDIENTS;

            // Liberar mutex del sistema
            sem_post(&s->mutex);

            // DEVOLVER el token: la orden sigue pendiente en la cola
            sem_post(&s->orders_sem);

            // Pequeña espera antes de reintentar (evita busy loop)
            usleep(300000);
            continue;
        }
        Order local = *o;
        o->state = IN_PROGRESS;
        s->queue_front = (s->queue_front + 1) % MAX_ORDERS;
        s->queue_count--;
        b->current_order = local.id;
        b->state = PREPARING;
        useIngredients(&b->available, &local.ingredients);
        printf("[PROC] Banda %d toma orden %d (pend:%d)\n", id, local.id, s->queue_count);
        sem_post(&s->mutex);

        sleep(1 + rand()%2);

        sem_wait(&s->mutex);
        b->burgers_processed++;
        b->current_order = -1;
        b->state = ACTIVE;
        s->completed_orders++;
        printf("[DONE] Banda %d completó orden %d (comp:%d)\n", id, local.id, s->completed_orders);
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
        // Procesar restock pedido externamente
        sem_wait(&shared_ctrl->mutex);
        int need_restock = shared_ctrl->restock_request;
        if (need_restock) shared_ctrl->restock_request = 0;
        sem_post(&shared_ctrl->mutex);

        if (need_restock) {
            sem_wait(&s->mutex);
            for (int i=0;i<s->band_count;i++)
                restockBand(s,i);  // reutiliza función
            sem_post(&s->mutex);
            printf("[RESTOCK] Stock repuesto\n");
        }

        sem_wait(&s->mutex);
        if (!s->running || !shared_ctrl->running) { sem_post(&s->mutex); break; }
        if (s->queue_count < MAX_ORDERS) {
            Order *o = &s->orders[s->queue_rear];
            o->id = id++;
            o->state = PENDING;
            o->ingredients = s->order_template;
            o->creation_time = time(NULL);
            s->queue_rear = (s->queue_rear + 1) % MAX_ORDERS;
            s->queue_count++;
            s->total_orders++;
            printf("[NEW] Orden %d (pend:%d/%d)\n", o->id, s->queue_count, MAX_ORDERS);
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

        printTableHeader();

        for (int i=0;i<s->band_count;i++) {
            sem_wait(&shared_ctrl->mutex);
            BandState ext = shared_ctrl->band_states[i];
            sem_post(&shared_ctrl->mutex);

            PreparationBand *b = &s->bands[i];

            // Estado efectivo mostrado
            const char *st;
            if (ext == STOPPED) st = "DETENIDO";
            else if (b->state == WAITING_FOR_INGREDIENTS) st = "EN ESPERA";
            else if (b->state == PREPARING) st = "PREPARANDO";
            else st = "ACTIVO";

            // Flag stock bajo (ejemplo si buns o meat < receta pedida)
            int low = (b->available.buns < s->order_template.buns ||
                       b->available.meat < s->order_template.meat);

            printf(" %2d   | %-10s | %4d | %5d | %4d %3d %3d %3d %4d %3d | %s\n",
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

        fflush(stdout);
        usleep(500000); // 0.5s
    }
    return NULL;
}

/* -------- Señal -------- */
static void handleSignal(int sig) {
    (void)sig;
    system_local.running = 0;
    if (shared_ctrl) shared_ctrl->running = 0;
    sem_post(&system_local.orders_sem);
}

/* -------- Main -------- */
int main(int argc, char *argv[]) {
    if (argc != 8) {
        printf("Uso: %s <bandas> <pan> <tomate> <cebolla> <lechuga> <carne> <queso>\n", argv[0]);
        return 1;
    }
    int bands = atoi(argv[1]);
    if (bands <1 || bands > MAX_BANDS) { printf("Bandas 1-%d\n", MAX_BANDS); return 1; }
    Ingredients tpl = { atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),
                        atoi(argv[5]),atoi(argv[6]),atoi(argv[7]) };
    if (tpl.buns<1 || tpl.meat<1) { printf("Pan y carne >=1\n"); return 1; }

    initSystem(&system_local, bands, tpl);
    createSharedControl(bands);

    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    band_threads = malloc(sizeof(pthread_t)*bands);
    for (int i=0;i<bands;i++)
        pthread_create(&band_threads[i], NULL, bandThread, (void*)(intptr_t)i);
    pthread_create(&gen_thread, NULL, orderGeneratorThread, NULL);
    pthread_create(&mon_thread, NULL, monitorThread, NULL);

    printf("Servidor listo.\nControl externo: ./control  (CTRL_SHM_KEY=%#x)\n", CTRL_SHM_KEY);
    printf("Comandos locales opcionales: 1-%d toggle | r restock | q salir\n", bands);

    while (system_local.running && shared_ctrl->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200 ms

        int ret = select(STDIN_FILENO+1, &rfds, NULL, NULL, &tv);

        // Revisa si control externo pidió parar mientras esperábamos
        if (!shared_ctrl->running) break;

        if (ret > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            int c = getchar();
            if (c == EOF) break;
            if (c == '\n') continue;

            if (c>='1' && c<='9') {
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
        // Si ret == 0 (timeout) simplemente vuelve a iterar y verifica flags
    }

    // Señal de cierre interna cuando control externo o local sale
    system_local.running = 0;
    shared_ctrl->running = 0;

    // Despertar hilos bloqueados en orders_sem
    for (int i=0; i<system_local.band_count; i++)
        sem_post(&system_local.orders_sem);

    for (int i=0;i<bands;i++) pthread_join(band_threads[i], NULL);
    pthread_join(gen_thread, NULL);
    pthread_join(mon_thread, NULL);

    sem_destroy(&shared_ctrl->mutex);
    shmdt(shared_ctrl);
    // shmctl(ctrl_shmid, IPC_RMID, NULL); // opcional

    sem_destroy(&system_local.mutex);
    sem_destroy(&system_local.orders_sem);
    free(band_threads);
    return 0;
}