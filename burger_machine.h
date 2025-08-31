#ifndef BURGER_MACHINE_H
#define BURGER_MACHINE_H

#include <time.h>
#include <semaphore.h>
#include <sys/types.h>

#define MAX_BANDS    9
#define MAX_ORDERS   15

// Clave para memoria compartida del control externo
#define CTRL_SHM_KEY 0x51544231

typedef enum { STOPPED, ACTIVE, WAITING_FOR_INGREDIENTS, PREPARING } BandState; // Estados de una banda

typedef struct { int buns,tomato,onion,lettuce,meat,cheese; } Ingredients; // Ingredientes

typedef struct {
    int id;
    Ingredients ingredients;
} Order;

typedef struct {
    int id;
    BandState state;
    Ingredients available;
    int burgers_processed;
    int current_order;
} PreparationBand;

typedef struct {
    int band_count;
    PreparationBand bands[MAX_BANDS];
    Order orders[MAX_ORDERS];
    int queue_front, queue_rear, queue_count; // campos para manejar la cola de ordenes
    int total_orders, completed_orders;
    Ingredients order_template;
    sem_t mutex; // mutex para proteger acceso a la estructura
    sem_t orders_sem; // semáforo para contar órdenes pendientes
    int running;
} BurgerSystem;

// Segmento mínimo compartido para control externo
typedef struct {
    int running;
    int band_count;
    BandState band_states[MAX_BANDS]; // STOPPED / ACTIVE (controla ejecución)
    int restock_request;              // Para incar que se debe hacer restock
    pid_t server_pid;                 // PID del proceso servidor (burger_machine) para enviar señales
    sem_t mutex;                      // Mutex para proteger acceso a esta estructura
} SharedControl;

#endif