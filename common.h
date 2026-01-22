#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

// konfiguracja simulation
#define NUM_TOURISTS 30
#define SIM_DURATION 30      // sekundy
#define VIP_PROBABILITY 1   // %

// konfiguracja kolejki
#define TOTAL_CHAIRS 72
#define MAX_BUSY_CHAIRS 36
#define CHAIR_CAPACITY 4
#define BIKERS_PER_CHAIR 2
#define WALKERS_WITH_BIKER 2

#define ENTRY_GATES 4
#define PLATFORM_GATES 3
#define EXIT_ROUTES 2
#define MAX_STATION_CAPACITY 10

#define RIDE_DURATION 5 

// ludzie
#define AGE_MIN 2
#define AGE_MAX 80
#define CHILD_AGE_LIMIT 10
#define SENIOR_AGE_LIMIT 65
#define GUARDIAN_AGE_MIN 18
#define CHILD_GUARDIAN_REQ_AGE 8

// trasy zjazdowe (w sekundach, dla symulacji)
#define ROUTE_TIME_T1 3  // Łatwa trasa
#define ROUTE_TIME_T2 5  // Średnia trasa
#define ROUTE_TIME_T3 7  // Trudna trasa

// struktury

// MAX_Q_SIZE: dla 10k procesów powinno być MIN(10000 + bufor, bezpieczna wartość)
// Aktualnie NUM_TOURISTS=30, MAX_Q_SIZE=31
// Dla 10k turystów: max jednocześnie w kolejce może być ~10000, ale w praktyce
// pracownik wysyła grupy, więc bufor 10000 + 10% = 11000 powinien wystarczyć
#define MAX_Q_SIZE (NUM_TOURISTS > 100 ? NUM_TOURISTS * 10 : NUM_TOURISTS + 1)

typedef enum {
    WALKER,
    BIKER
} TouristType;

typedef enum {
    SINGLE, TK1, TK2, TK3, DAILY
} TicketType;


typedef struct {
    pid_t pid;
    TouristType type;
    int age;
    int requires_guardian;
    int guardian_id;  // PID opiekuna (jeśli jest)
} QueuedTourist;

typedef struct {
    QueuedTourist items[MAX_Q_SIZE];
    int head;
    int tail;
    int count;
} Queue;

// Shared FIFO queue for tourists waiting to board
typedef struct {
    QueuedTourist items[MAX_Q_SIZE];
    int head;
    int tail;
    int count;
} SharedQueue;

typedef struct {
    volatile int is_running;
    time_t start_time;
    time_t end_time;
    int busy_chairs;
    int station_population;
    SharedQueue waiting_tourists_down;
    SharedQueue waiting_tourists_up;
} StationState;

typedef struct {
    pid_t owner_pid;
    TicketType type;
    int validation_count;
    time_t valid_until;
} Ticket;

typedef struct {
    int single_sold;
    int tk1_sold;
    int tk2_sold;
    int tk3_sold;
    int daily_sold;
    int total_rides;
    int bikers;
    int walkers;
    int vip_served;
    int with_guardian;
    int route_t1;  // liczba turystów na trasie T1
    int route_t2;  // liczba turystów na trasie T2
    int route_t3;  // liczba turystów na trasie T3
} DailyStats;

// komunikacja

// Typy wiadomości
#define MSG_TYPE_LOG 1
#define MSG_TYPE_CASHIER_REQ 2
#define MSG_TYPE_WORKER_DOWN 100    // Kanał dla pracownika na dole
#define MSG_TYPE_WORKER_UP 101      // Kanał dla pracownika na górze

// Akcje w wiadomościach do pracownika
typedef enum {
    ACTION_TOURIST_READY,
    ACTION_CHAIR_FREE
} WorkerMsgAction;

// Struktura wiadomości
typedef struct {
    long mtype; // PID turysty LUB jeden z powyższych typów
    
    pid_t pid;
    int tourist_id;
    TouristType tourist_type;
    int age;
    int is_vip;
    int requires_guardian;
    pid_t guardian_pid;

    WorkerMsgAction action; 
    char mtext[128];
} MsgBuf;

// ========== GLOBALNE ZMIENNE IPC ==========
// Deklaracje `extern` mówią kompilatorowi, że te zmienne istnieją,
// ale są zdefiniowane w innym pliku (.c). Zapobiega to błędom "multiple definition".
extern int msg_queue_id;
extern int shm_state_id, shm_tickets_id, shm_stats_id;
extern int sem_state_mutex_id, sem_chair_queue_id, sem_chair_loaded_id;
extern int sem_entry_id, sem_station_id, sem_platform_id, sem_exit_id;
extern int sem_ticket_bought_id;

extern StationState *state;
extern Ticket *tickets;
extern DailyStats *stats;


// ========== PROTOTYPY FUNKCJI ==========

// --- utils.c ---
void sem_op(int sem_id, int sem_num, int op);
void sem_wait(int sem_id, int sem_num);
void sem_signal(int sem_id, int sem_num);

void q_init(Queue *q);
int q_push(Queue *q, QueuedTourist t);
QueuedTourist q_pop(Queue *q);
int q_is_empty(Queue *q);

void attach_ipc_resources();
void cleanup_ipc();

void log_msg(const char *format, ...);


#endif // COMMON_H
