#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <semaphore.h>
#include <signal.h>
#include "burger_machine.h"

int main() {
    int shmid = shmget(CTRL_SHM_KEY, sizeof(SharedControl), 0666);
    if (shmid == -1) { perror("shmget control"); return 1; }
    SharedControl *c = (SharedControl*)shmat(shmid, NULL, 0);
    if (c == (void*)-1) { perror("shmat control"); return 1; }

    printf("Control externo conectado. Bandas:%d\n", c->band_count);
    printf("Comandos: 1-%d pausar bandar | r restock | q salir\n", c->band_count);

    int ch;
    while (c->running && (ch=getchar())!=EOF) {
        if (ch=='\n') continue;
        if (ch>='1' && ch<='9') {
            int id = ch-'1';
            if (id < c->band_count) {
                sem_wait(&c->mutex);
                c->band_states[id] =
                   (c->band_states[id]==STOPPED)?ACTIVE:STOPPED;
                BandState st = c->band_states[id];
                sem_post(&c->mutex);
                printf("[CTRL] Banda %d -> %s\n", id+1,
                       st==ACTIVE?"ACTIVA":"DETENIDA");
            }
        } else if (ch=='r' || ch=='R') {
            sem_wait(&c->mutex);
            c->restock_request = 1;
            sem_post(&c->mutex);
            printf("[CTRL] Restock solicitado\n");
        } else if (ch=='q' || ch=='Q') {
            sem_wait(&c->mutex);
            c->running = 0;
            pid_t pid = c->server_pid;
            sem_post(&c->mutex);
            printf("[CTRL] Parada solicitada\n");
            if (pid > 0) kill(pid, SIGINT);  // notifica al servidor
            break;
        }
    }
    shmdt(c);
    return 0;
}