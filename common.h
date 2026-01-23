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
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>

// ============ KONFIGURACJA SYMULACJI ============
#define NUM_TOURISTS 200
#define SIM_DURATION 120
#define CLOSING_TIME 90
#define VIP_PROBABILITY 1       // ~1% szansa na VIP (wg opisu: ok. 1%)

// ============ KONFIGURACJA KOLEI ============
#define TOTAL_CHAIRS 72
#define MAX_BUSY_CHAIRS 36      // jednoczesnie moze byc zajetych 36 krzeselek
#define CHAIR_CAPACITY 4        // max 4 osoby na krzesle
#define MAX_BIKERS_PER_CHAIR 2  // max 2 rowerzystow na krzesle
#define ENTRY_GATES 4           // 4 bramki wejsciowe (sprawdzanie biletu)
#define PLATFORM_GATES 3        // 3 bramki peronowe (kontrola grupy, otwiera pracownik1)
#define EXIT_ROUTES 2           // 2 drogi wyjscia ze stacji gornej
#define MAX_STATION_CAPACITY 30 // max N osob na terenie stacji dolnej
#define RIDE_DURATION 5         // czas przejazdu w gore

// ============ KONFIGURACJA OSOB ============
#define AGE_MIN 4
#define AGE_MAX 80
#define CHILD_NEEDS_GUARDIAN_MAX 9   // dzieci <= 8 lat pod opieka doroslego (wg opisu: od 4 do 8 lat)
#define CHILD_DISCOUNT_MAX 10        // dzieci < 10 lat maja znizke
#define SENIOR_AGE_LIMIT 66          // seniorzy powyzej 65. roku zycia (>65, tj. od 66 lat)
#define GUARDIAN_AGE_MIN 18          // opiekun musi miec min 18 lat
#define MAX_CHILDREN_PER_GUARDIAN 2  // max 2 dzieci pod opieka jednego doroslego

// ============ TRASY ZJAZDOWE ============
#define ROUTE_TIME_T1 3     // trasa latwa (T1 < T2 < T3)
#define ROUTE_TIME_T2 5     // trasa srednia
#define ROUTE_TIME_T3 7     // trasa trudna

// ============ BATCH PROCESOW ============
#define BATCH_SIZE 100

// ============ FIFO DO KOMUNIKACJI MIEDZY PRACOWNIKAMI ============
#define FIFO_WORKER_PATH "/tmp/kolej_worker_fifo"

// ============ TYPY ============
typedef enum {
    WALKER,
    BIKER
} TouristType;

typedef enum {
    SINGLE,     // jednorazowy
    TK1,        // czasowy 1h
    TK2,        // czasowy 2h
    TK3,        // czasowy 3h
    DAILY       // dzienny
} TicketType;

// ============ STRUKTURY KOLEJKI ============
#define MAX_Q_SIZE 512

typedef struct {
    pid_t pid;
    TouristType type;
    int age;
    int requires_guardian;
    int guardian_id;
    int is_removed;
    int num_children;
    int tourist_id;
    int family_id;
    int is_vip;
} QueuedTourist;

typedef struct {
    QueuedTourist items[MAX_Q_SIZE];
    int head;
    int tail;
    int count;
} SharedQueue;

// ============ REJESTRACJA PRZEJSC BRAMKOWYCH ============
#define MAX_PASS_LOG 1024

typedef struct {
    int tourist_id;
    TicketType ticket_type;
    time_t timestamp;
    int ride_number;    // ktory przejazd z kolei
} PassLogEntry;

// ============ STAN STACJI (PAMIEC DZIELONA) ============
typedef struct {
    volatile int is_running;
    volatile int is_closing;
    volatile int is_paused;         // kolej zatrzymana przez pracownika
    time_t start_time;
    time_t end_time;
    int busy_chairs;                // liczba zajetych KRZESELEK (nie osob!)
    int station_population;         // ile osob na terenie stacji dolnej
    pid_t worker_down_pid;          // PID pracownika1 (stacja dolna)
    pid_t worker_up_pid;            // PID pracownika2 (stacja gorna)
    SharedQueue waiting_tourists_down;  // kolejka oczekujacych na dole
    int next_family_id;
    // Rejestracja przejsc bramkowych
    PassLogEntry pass_log[MAX_PASS_LOG];
    int pass_log_count;
    // Statystyki osob na gorze (czekajacych na wyjscie)
    int people_on_top;
} StationState;

// ============ BILET ============
typedef struct {
    pid_t owner_pid;
    TicketType type;
    int validation_count;       // ile razy uzyty
    time_t valid_until;         // -1 = bez limitu czasowego
    int family_id;
    int is_valid;               // czy bilet aktywny
} Ticket;

// ============ STATYSTYKI DZIENNE ============
typedef struct {
    int single_sold;
    int tk1_sold;
    int tk2_sold;
    int tk3_sold;
    int daily_sold;
    int total_rides;        // laczna liczba odjazdow krzeselek
    int total_people;       // laczna liczba przewiezionych osob
    int bikers;
    int walkers;
    int vip_served;
    int with_guardian;
    int route_t1;
    int route_t2;
    int route_t3;
    int rejected_expired;   // odrzuceni z powodu wygasniecia karnetu
} DailyStats;

// ============ KOMUNIKATY (KOLEJKA KOMUNIKATOW) ============
#define MSG_TYPE_LOG 1
#define MSG_TYPE_CASHIER_REQ 2
#define MSG_TYPE_WORKER_DOWN 100
#define MSG_TYPE_WORKER_UP 101

typedef enum {
    ACTION_TOURIST_READY,
    ACTION_CHAIR_FREE,
    ACTION_ARRIVED_TOP,         // turysta dotarl na gore
    ACTION_REJECTED = -1
} WorkerMsgAction;

typedef struct {
    long mtype;
    pid_t pid;
    int tourist_id;
    TouristType tourist_type;
    int age;
    int is_vip;
    int requires_guardian;
    pid_t guardian_pid;
    int num_children;
    int family_id;
    WorkerMsgAction action;
    char mtext[128];
} MsgBuf;

// ============ KOMUNIKATY FIFO MIEDZY PRACOWNIKAMI ============
typedef enum {
    FIFO_PAUSE_REQ,     // zadanie zatrzymania kolei
    FIFO_PAUSE_ACK,     // potwierdzenie gotowosci do zatrzymania
    FIFO_RESUME_REQ,    // zadanie wznowienia
    FIFO_RESUME_ACK     // potwierdzenie wznowienia
} FifoMsgType;

typedef struct {
    FifoMsgType type;
    pid_t sender_pid;
} FifoMsg;

// ========== GLOBALNE ZMIENNE IPC ==========
extern int msg_queue_id;
extern int shm_state_id, shm_tickets_id, shm_stats_id;
extern int sem_state_mutex_id, sem_chair_queue_id, sem_chair_loaded_id;
extern int sem_entry_id, sem_station_id, sem_platform_id, sem_exit_id;
extern int sem_ticket_bought_id;
extern int sem_worker_wakeup_down_id;
extern StationState *state;
extern Ticket *tickets;
extern DailyStats *stats;

// ========== PROTOTYPY FUNKCJI ==========
void sem_op(int sem_id, int sem_num, int op);
void sem_wait_op(int sem_id, int sem_num);
void sem_signal_op(int sem_id, int sem_num);
int sem_trywait_op(int sem_id, int sem_num);

void attach_ipc_resources();
void cleanup_ipc();
void log_msg(const char *format, ...);

#endif // COMMON_H
