#include "common.h"
#include <stdarg.h>

int msg_queue_id;
int shm_state_id, shm_tickets_id, shm_stats_id;
int sem_state_mutex_id, sem_chair_queue_id, sem_chair_loaded_id;
int sem_entry_id, sem_station_id, sem_platform_id, sem_exit_id;
int sem_ticket_bought_id;

StationState *state;
Ticket *tickets;
DailyStats *stats;

// operacje semaforów
void sem_op(int sem_id, int sem_num, int op) {
    struct sembuf sops;
    sops.sem_num = sem_num;
    sops.sem_op = op;
    sops.sem_flg = 0;
    if (semop(sem_id, &sops, 1) == -1) {
        if (errno != EIDRM && errno != EINVAL) {
            perror("semop error");
        }
    }
}

void sem_wait(int sem_id, int sem_num) {
    sem_op(sem_id, sem_num, -1);
}

void sem_signal(int sem_id, int sem_num) {
    sem_op(sem_id, sem_num, 1);
}

// Kolejki
void q_init(Queue *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

int q_push(Queue *q, QueuedTourist t) {
    if (q->count >= MAX_Q_SIZE) {
        return -1;
    }
    q->items[q->tail] = t;
    q->tail = (q->tail + 1) % MAX_Q_SIZE;
    q->count++;
    return 0;
}

QueuedTourist q_pop(Queue *q) {
    if (q->count <= 0) {
        QueuedTourist empty = { -1, -1 };
        return empty;
    }
    QueuedTourist t = q->items[q->head];
    q->head = (q->head + 1) % MAX_Q_SIZE;
    q->count--;
    return t;
}

int q_is_empty(Queue *q) {
    return q->count == 0;
}

//IPC
void attach_ipc_resources() {
    // Kolejka komunikatów
    key_t key_msg = ftok(".", 'Q');
    msg_queue_id = msgget(key_msg, 0666);
    if (msg_queue_id == -1) { perror("msgget attach"); exit(1); }

    // Pamięć dzielona
    key_t key_state = ftok(".", 'S');
    shm_state_id = shmget(key_state, sizeof(StationState), 0666);
    if (shm_state_id == -1) { perror("shmget attach state"); exit(1); }
    state = (StationState *)shmat(shm_state_id, NULL, 0);
    if (state == (void *)-1) { perror("shmat attach state"); exit(1); }

    key_t key_tickets = ftok(".", 'K');
    shm_tickets_id = shmget(key_tickets, sizeof(Ticket) * (NUM_TOURISTS + 1), 0666);
    if (shm_tickets_id == -1) { perror("shmget attach tickets"); exit(1); }
    tickets = (Ticket *)shmat(shm_tickets_id, NULL, 0);
    if (tickets == (void *)-1) { perror("shmat attach tickets"); exit(1); }

    key_t key_stats = ftok(".", 'T');
    shm_stats_id = shmget(key_stats, sizeof(DailyStats), 0666);
    if (shm_stats_id == -1) { perror("shmget attach stats"); exit(1); }
    stats = (DailyStats *)shmat(shm_stats_id, NULL, 0);
    if (stats == (void *)-1) { perror("shmat attach stats"); exit(1); }

    // Semafory
    sem_state_mutex_id = semget(ftok(".", 'M'), 1, 0666);
    if (sem_state_mutex_id == -1) { perror("semget attach state_mutex"); exit(1); }

    sem_chair_queue_id = semget(ftok(".", 'C'), 1, 0666);
    if (sem_chair_queue_id == -1) { perror("semget attach chair_queue"); exit(1); }

    sem_chair_loaded_id = semget(ftok(".", 'L'), 1, 0666);
    if (sem_chair_loaded_id == -1) { perror("semget attach chair_loaded"); exit(1); }

    sem_entry_id = semget(ftok(".", 'E'), ENTRY_GATES, 0666);
    if (sem_entry_id == -1) { perror("semget attach entry"); exit(1); }

    sem_station_id = semget(ftok(".", 'A'), 1, 0666);
    if (sem_station_id == -1) { perror("semget attach station"); exit(1); }

    sem_platform_id = semget(ftok(".", 'P'), PLATFORM_GATES, 0666);
     if (sem_platform_id == -1) { perror("semget attach platform"); exit(1); }

    sem_exit_id = semget(ftok(".", 'X'), EXIT_ROUTES, 0666);
    if (sem_exit_id == -1) { perror("semget attach exit"); exit(1); }

    sem_ticket_bought_id = semget(ftok(".", 'B'), NUM_TOURISTS, 0666);
    if (sem_ticket_bought_id == -1) { perror("semget attach ticket_bought"); exit(1); }
}

void cleanup_ipc() {
    printf("SYSTEM: Sprzątanie zasobów IPC...\n");

    shmdt(state);
    shmdt(tickets);
    shmdt(stats);

    msgctl(msg_queue_id, IPC_RMID, NULL);
    shmctl(shm_state_id, IPC_RMID, NULL);
    shmctl(shm_tickets_id, IPC_RMID, NULL);
    shmctl(shm_stats_id, IPC_RMID, NULL);
    semctl(sem_state_mutex_id, 0, IPC_RMID);
    semctl(sem_chair_queue_id, 0, IPC_RMID);
    semctl(sem_chair_loaded_id, 0, IPC_RMID);
    semctl(sem_entry_id, 0, IPC_RMID);
    semctl(sem_station_id, 0, IPC_RMID);
    semctl(sem_platform_id, 0, IPC_RMID);
    semctl(sem_exit_id, 0, IPC_RMID);
    semctl(sem_ticket_bought_id, 0, IPC_RMID);
    
    printf("SYSTEM: Zasoby IPC posprzątane.\n");
}


// logowanie
void log_msg(const char *format, ...) {
    MsgBuf msg;
    msg.mtype = MSG_TYPE_LOG;
    msg.pid = getpid();

    va_list args;
    va_start(args, format);
    vsnprintf(msg.mtext, sizeof(msg.mtext), format, args);
    va_end(args);

    time_t now = time(NULL);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));
    printf("[%s] [PID: %d] %s\n", time_str, msg.pid, msg.mtext);


    if (msgsnd(msg_queue_id, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
    }
}
