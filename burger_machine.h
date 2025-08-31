#ifndef BURGER_MACHINE_H
#define BURGER_MACHINE_H

#include <time.h>
#include <semaphore.h>
#include <sys/types.h>

#define MAX_BANDS    9
#define MAX_ORDERS   15

// Clave para memoria compartida del control externo
#define CTRL_SHM_KEY 0x51544231

typedef enum { STOPPED, ACTIVE, WAITING_FOR_INGREDIENTS, PREPARING } BandState;
typedef enum { PENDING, IN_PROGRESS, COMPLETED } OrderState;

typedef struct { int buns,tomato,onion,lettuce,meat,cheese; } Ingredients;

typedef struct {
    int id;
    OrderState state;
    Ingredients ingredients;
    time_t creation_time;
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
    int queue_front, queue_rear, queue_count;
    int total_orders, completed_orders;
    Ingredients order_template;
    sem_t mutex;
    sem_t orders_sem;
    int running;
} BurgerSystem;

// Segmento mínimo compartido para control externo
typedef struct {
    int running;
    int band_count;
    BandState band_states[MAX_BANDS]; // STOPPED / ACTIVE (controla ejecución)
    int restock_request;              // 1 => servidor debe hacer restock y poner 0
    pid_t server_pid;                 // asegúrate de que control.c use esto
    sem_t mutex;                      // pshared=1
} SharedControl;

#endif