#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>

// Enumerations for states
typedef enum {
    STOPPED,                // Banda detenida
    ACTIVE,                 // Banda activa
    WAITING_FOR_INGREDIENTS,// Esperando ingredientes
    PREPARING               // Preparando hamburguesa
} BandState;

typedef enum {
    PENDING,                // Orden pendiente
    IN_PROGRESS,            // Orden en proceso
    COMPLETED               // Orden completada
} OrderState;

// Structure for ingredients
typedef struct {
    int buns;               // Panes
    int tomato;             // Tomate
    int onion;              // Cebolla
    int lettuce;            // Lechuga
    int meat;               // Carne
    int cheese;             // Queso
} Ingredients;

// Structure for an order
typedef struct {
    int id;                 // ID de la orden
    OrderState state;       // Estado de la orden
    Ingredients ingredients; // Ingredientes requeridos
    time_t creation_time;   // Tiempo de creación
} Order;

// Structure for a preparation band
typedef struct {
    int id;                         // ID de la banda
    BandState state;                // Estado de la banda
    Ingredients available_ingredients; // Ingredientes disponibles
    int burgers_processed;          // Contador de hamburguesas procesadas
    int current_order;              // ID de la orden actual (-1 si no hay)
} PreparationBand;

// Main system structure
typedef struct {
    int band_count;                 // Número de bandas
    PreparationBand *bands;         // Array de bandas
    Order *pending_orders;          // Cola de órdenes pendientes
    int total_orders;               // Total de órdenes recibidas
    int completed_orders;           // Órdenes completadas
    int max_orders;                 // Capacidad máxima de órdenes
    int queue_front;                // Frente de la cola
    int queue_rear;                 // Final de la cola
    sem_t mutex;                    // Semáforo para exclusión mutua
    sem_t orders_sem;               // Semáforo para control de órdenes
    int running;                    // Flag de ejecución del sistema
} BurgerSystem;

// Global system variable
BurgerSystem global_system;

// Global thread identifiers
pthread_t *band_threads;
pthread_t order_thread;
pthread_t monitor_thread;

// Show usage help
void showHelp() {
    printf("\nParámetros:\n");
    printf("  <número de bandas>: Número de bandas de preparación (1-9)\n");
    printf("  <máximo de órdenes>: Capacidad máxima de órdenes pendientes\n");
    printf("\nEjemplo: ./burger_machine 3 20\n");
}

// Check if there are enough ingredients
int checkIngredients(Ingredients *available, Ingredients *required) {
    printf("Verificando ingredientes: Disponibles (P:%d T:%d C:%d L:%d Ca:%d Q:%d), Requeridos (P:%d T:%d C:%d L:%d Ca:%d Q:%d)\n",
           available->buns, available->tomato, available->onion, available->lettuce, available->meat, available->cheese,
           required->buns, required->tomato, required->onion, required->lettuce, required->meat, required->cheese);
    return (available->buns >= required->buns &&
            available->tomato >= required->tomato &&
            available->onion >= required->onion &&
            available->lettuce >= required->lettuce &&
            available->meat >= required->meat &&
            available->cheese >= required->cheese);
}

// Use ingredients to prepare a burger
void useIngredients(Ingredients *available, Ingredients *required) {
    available->buns -= required->buns;
    available->tomato -= required->tomato;
    available->onion -= required->onion;
    available->lettuce -= required->lettuce;
    available->meat -= required->meat;
    available->cheese -= required->cheese;
}

// Generate random ingredients for an order
Ingredients generateRandomIngredients() {
    Ingredients ing;
    ing.buns = 1 + rand() % 2;      // 1-2 panes
    ing.tomato = rand() % 2;        // 0-1 tomates
    ing.onion = rand() % 2;         // 0-1 cebollas
    ing.lettuce = rand() % 2;       // 0-1 lechugas
    ing.meat = 1;                   // Siempre 1 carne
    ing.cheese = rand() % 2;        // 0-1 quesos
    return ing;
}

// Restock ingredients in a band
void restockIngredients(BurgerSystem *system, int band_id) {
    sem_wait(&system->mutex);
    PreparationBand *band = &system->bands[band_id];
    // Restock determinista (antes era aleatorio y confundía)
    const int ADD_BUNS = 10, ADD_TOM = 5, ADD_ONI = 5, ADD_LET = 5, ADD_MEAT = 10, ADD_CHE = 5;
    band->available_ingredients.buns   += ADD_BUNS;
    band->available_ingredients.tomato += ADD_TOM;
    band->available_ingredients.onion  += ADD_ONI;
    band->available_ingredients.lettuce+= ADD_LET;
    band->available_ingredients.meat   += ADD_MEAT;
    band->available_ingredients.cheese += ADD_CHE;
    if (band->state == WAITING_FOR_INGREDIENTS) band->state = ACTIVE;
    printf("[RESTOCK] Banda %d -> P:%d T:%d C:%d L:%d Ca:%d Q:%d\n", band_id,
           band->available_ingredients.buns,
           band->available_ingredients.tomato,
           band->available_ingredients.onion,
           band->available_ingredients.lettuce,
           band->available_ingredients.meat,
           band->available_ingredients.cheese);
    sem_post(&system->mutex);
}

// Initialize system without shared memory
void initializeSystem(BurgerSystem *system, int band_count, int max_orders) {
    system->band_count = band_count;
    system->max_orders = max_orders;
    system->total_orders = 0; 
    system->completed_orders = 0;
    system->queue_front = 0;
    system->queue_rear = 0;
    system->running = 1;

    // Allocate memory for bands
    system->bands = (PreparationBand*)malloc(band_count * sizeof(PreparationBand));
    if (system->bands == NULL) {
        perror("Error al asignar memoria para las bandas");
        exit(EXIT_FAILURE);
    }

    // Allocate memory for orders
    system->pending_orders = (Order*)malloc(max_orders * sizeof(Order));
    if (system->pending_orders == NULL) {
        perror("Error al asignar memoria para las órdenes");
        free(system->bands);
        exit(EXIT_FAILURE);
    }

    // Initialize semaphores
    sem_init(&system->mutex, 0, 1);  // 0 indica que es para hilos, no procesos
    sem_init(&system->orders_sem, 0, 0);

    // Initialize bands
    srand(time(NULL));
    for (int i = 0; i < band_count; i++) {
        system->bands[i].id = i;
        system->bands[i].state = ACTIVE;
        system->bands[i].burgers_processed = 0;
        system->bands[i].current_order = -1;

        // Asignar valores iniciales altos a los ingredientes
        system->bands[i].available_ingredients.buns = 5;     // 5 panes
        system->bands[i].available_ingredients.tomato = 5;   // 5 tomates
        system->bands[i].available_ingredients.onion = 5;    // 5 cebollas
        system->bands[i].available_ingredients.lettuce = 5;  // 5 lechugas
        system->bands[i].available_ingredients.meat = 5;     // 5 carnes
        system->bands[i].available_ingredients.cheese = 5;   // 5 quesos
    }
}

// Free system resources
void destroySystem(BurgerSystem *system) {
    sem_destroy(&system->mutex);
    sem_destroy(&system->orders_sem);
    free(system->bands);
    free(system->pending_orders);
}

// Process for each preparation band
void bandProcess(BurgerSystem *system, int band_id) {
    PreparationBand *band = &system->bands[band_id];
    while (system->running) {
        if (!system->running) break;

        // Esperar hasta que exista al menos una orden disponible
        if (sem_wait(&system->orders_sem) == -1) {
            if (!system->running) break;
        }
        if (!system->running) break;

        sem_wait(&system->mutex);
        if (!system->running) { sem_post(&system->mutex); break; }

        // Si la banda está detenida, devolver el crédito del semáforo y continuar
        if (band->state == STOPPED) {
            sem_post(&system->mutex);
            sem_post(&system->orders_sem); // devolver la orden para otra banda
            sleep(1);
            continue;
        }

        // Verificar que realmente haya una orden (otra banda pudo tomarla en carrera)
        if (system->queue_front == system->queue_rear) {
            sem_post(&system->mutex);
            continue; // volver a esperar
        }

        int order_idx = system->queue_front;
        Order *slot = &system->pending_orders[order_idx];

        // Si la orden no está pendiente, liberar y continuar (no se reinyecta semáforo porque ya fue consumido por quien la tomó)
        if (slot->state != PENDING) {
            sem_post(&system->mutex);
            continue;
        }

        // Verificar ingredientes antes de desencolar
        if (!checkIngredients(&band->available_ingredients, &slot->ingredients)) {
            band->state = WAITING_FOR_INGREDIENTS;
            printf("[FALTA] Banda %d no puede procesar orden %d (P:%d T:%d C:%d L:%d Ca:%d Q:%d). Esperando restock manual.\n",
                   band_id, slot->id,
                   band->available_ingredients.buns,
                   band->available_ingredients.tomato,
                   band->available_ingredients.onion,
                   band->available_ingredients.lettuce,
                   band->available_ingredients.meat,
                   band->available_ingredients.cheese);
            // Devolver crédito porque la orden sigue en cola
            sem_post(&system->mutex);
            sem_post(&system->orders_sem);
            sleep(1);
            continue;
        }

        // Reclamar (desencolar) la orden inmediatamente para permitir que otras bandas avancen
        Order local = *slot; // copia local para trabajar fuera de la sección crítica
        slot->state = IN_PROGRESS;
        system->queue_front = (system->queue_front + 1) % system->max_orders;
        band->current_order = local.id;
        band->state = PREPARING;
        useIngredients(&band->available_ingredients, &local.ingredients);
        printf("[PROC] Banda %d toma orden %d. Pendientes ahora: %d\n", band_id, local.id,
               (system->queue_rear - system->queue_front + system->max_orders) % system->max_orders);
        sem_post(&system->mutex);

        // Simular preparación
        sleep(2 + rand() % 3);

        sem_wait(&system->mutex);
        if (!system->running) { sem_post(&system->mutex); break; }
        band->burgers_processed++;
        band->current_order = -1;
        band->state = ACTIVE;
        system->completed_orders++;
        printf("[DONE] Banda %d completó orden %d. Completadas: %d\n", band_id, local.id, system->completed_orders);
        sem_post(&system->mutex);
    }
}

// Order generator process
void orderGeneratorProcess(BurgerSystem *system) {
    int order_id = 1; // Iniciar desde 1
    
    while (system->running) {
        sleep(1 + rand() % 3);

        sem_wait(&system->mutex);
        if (!system->running) {
            sem_post(&system->mutex);
            break;
        }

        // Verificar si hay espacio en la cola
        if (((system->queue_rear + 1) % system->max_orders) != system->queue_front) {
            Order *order = &system->pending_orders[system->queue_rear];
            order->id = order_id++;
            order->state = PENDING;
            order->ingredients = generateRandomIngredients();
            order->creation_time = time(NULL);

            // Actualizar queue_rear
            system->queue_rear = (system->queue_rear + 1) % system->max_orders;
            system->total_orders++;

            printf("Orden generada: ID=%d, queue_rear=%d, queue_front=%d\n", order->id, system->queue_rear, system->queue_front);

            sem_post(&system->orders_sem);
        } else {
            printf("Cola llena: no se puede generar más órdenes.\n");
        }
        sem_post(&system->mutex);
    }
}

// Monitoring interface
void monitoringInterface(BurgerSystem *system) {
    while (system->running) {
        printf("\033[H\033[J");
        printf("=== SISTEMA AUTOMATIZADO DE PREPARACIÓN DE HAMBURGUESAS ===\n\n");

        sem_wait(&system->mutex);
        if (!system->running) { // Verificar después de sem_wait
            sem_post(&system->mutex);
            break;
        }

        printf("BANDAS DE PREPARACIÓN:\n");
        printf("ID | Estado        | Hamburguesas | Ingredientes (P/T/C/L/Ca/Q)\n");
        printf("---------------------------------------------------------------\n");

        for (int i = 0; i < system->band_count; i++) {
            PreparationBand *band = &system->bands[i];
            const char *state_str;
            switch (band->state) {
                case STOPPED: state_str = "DETENIDA"; break;
                case ACTIVE: state_str = "ACTIVA"; break;
                case WAITING_FOR_INGREDIENTS: state_str = "ESPERANDO"; break;
                case PREPARING: state_str = "PREPARANDO"; break;
                default: state_str = "DESCONOCIDO";
            }
            printf("%2d | %-12s | %12d | %2d/%2d/%2d/%2d/%2d/%2d\n", 
                   band->id, state_str, band->burgers_processed,
                   band->available_ingredients.buns,
                   band->available_ingredients.tomato,
                   band->available_ingredients.onion,
                   band->available_ingredients.lettuce,
                   band->available_ingredients.meat,
                   band->available_ingredients.cheese);
        }

        printf("\nÓRDENES: Total: %d, Completadas: %d, Pendientes: %d\n", 
               system->total_orders, system->completed_orders,
               (system->queue_rear - system->queue_front + system->max_orders) % system->max_orders);

        sem_post(&system->mutex);
        sleep(1);
    }
}

// Wrappers compatibles con pthread
static void* bandThread(void *arg) {
    int band_id = (int)(intptr_t)arg;
    bandProcess(&global_system, band_id);
    return NULL;
}

static void* orderGeneratorThread(void *arg) {
    (void)arg;
    orderGeneratorProcess(&global_system);
    return NULL;
}

static void* monitoringThread(void *arg) {
    (void)arg;
    monitoringInterface(&global_system);
    return NULL;
}

// Control interface
void controlInterface(BurgerSystem *system) {
    printf("Controles:\n");
    printf("  [1-%d] : Alternar banda (activar/desactivar)\n", system->band_count);
    printf("  'r'   : Reponer ingredientes en todas las bandas\n");
    printf("  'q'   : Salir\n");
    
    char command;
    while (system->running) {
        command = getchar();
        
        if (command >= '1' && command <= '9') {
            int band_id = command - '1';
            if (band_id < system->band_count) {
                sem_wait(&system->mutex);
                if (system->bands[band_id].state == STOPPED) {
                    system->bands[band_id].state = ACTIVE;
                    printf("Banda %d activada\n", band_id + 1);
                } else {
                    system->bands[band_id].state = STOPPED;
                    printf("Banda %d detenida\n", band_id + 1);
                }
                sem_post(&system->mutex);
            }
        } else if (command == 'r' || command == 'R') {
            for (int i = 0; i < system->band_count; i++) {
                restockIngredients(system, i);
            }
            printf("Ingredientes repuestos en todas las bandas\n");
        } else if (command == 'q' || command == 'Q') {
            system->running = 0;
            printf("Saliendo...\n");
        }
    }
}

// Signal handler for graceful termination
void handleSignal(int sig) {
    printf("\nSeñal %d recibida, terminando ejecución...\n", sig);
    global_system.running = 0;

    // Cancelar hilos directamente
    for (int i = 0; i < global_system.band_count; i++) {
        pthread_cancel(band_threads[i]);
    }
    pthread_cancel(order_thread);
    pthread_cancel(monitor_thread);

    // Desbloquear semáforos para evitar bloqueos
    sem_post(&global_system.mutex);
    sem_post(&global_system.orders_sem);
}

// Main function
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Uso: %s <número de bandas> <máximo de órdenes>\n", argv[0]);
        showHelp();
        return 1;
    }

    int band_count = atoi(argv[1]);
    int max_orders = atoi(argv[2]);

    if (band_count <= 0 || max_orders <= 0) {
        printf("Error: ambos parámetros deben ser números positivos\n");
        showHelp();
        return 1;
    }

    // Configure signal handler
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    initializeSystem(&global_system, band_count, max_orders);

    // Create threads for preparation bands (usar wrapper correcto)
    band_threads = malloc(band_count * sizeof(pthread_t));
    for (int i = 0; i < band_count; i++) {
        pthread_create(&band_threads[i], NULL, bandThread, (void*)(intptr_t)i);
    }

    // Create thread for order generator (wrapper)
    pthread_create(&order_thread, NULL, orderGeneratorThread, NULL);

    // Create thread for monitoring interface (wrapper)
    pthread_create(&monitor_thread, NULL, monitoringThread, NULL);

    // Control interface in the main thread
    controlInterface(&global_system);

    // Wait for threads to finish
    for (int i = 0; i < band_count; i++) {
        pthread_join(band_threads[i], NULL);
    }
    pthread_join(order_thread, NULL);
    pthread_join(monitor_thread, NULL);

    destroySystem(&global_system);
    free(band_threads);
    return 0;
}