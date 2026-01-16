// main.c
#define _POSIX_C_SOURCE 200809L 

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <sys/msg.h>

#include <signal.h>
#include <errno.h>

#define TOTAL_CHAIRS 72
#define MAX_BUSY_CHAIRS 36
#define NUM_TOURISTS 30
#define SIM_DURATION 40
#define MAX_STATION_CAPACITY 50
#define ENTRY_GATES 4 
#define PLATFORM_GATES 3
#define EXIT_ROUTES 2

#define TRAIL_T1 5 
#define TRAIL_T2 8
#define TRAIL_T3 12

#define TICKET_SINGLE 0
#define TICKET_TK1 1
#define TICKET_TK2 2
#define TICKET_TK3 3
#define TICKET_DAILY 4

// Typy komunikatów
#define MSG_TYPE_KASJER 1
#define MSG_TYPE_WORKER_CTRL 2
#define MSG_TYPE_WORKER_ACK 3
#define MSG_TYPE_CHAIR_READY 4
#define MSG_TYPE_CHAIR_LOADED 5
#define MSG_TYPE_TOURIST_BASE 100


// Struktury danych
typedef struct {
    int bikers;
    int walkers;
    int total_occupants;
} ChairOccupancy;

typedef struct {
    int is_running;
    int busy_chairs;
    int people_in_station;
    int people_on_platform;
    int stop_selling;
    time_t start_time;
    time_t end_time;
    int emergency_stop;
    int total_passes;
    int waiting_for_ack;
    pid_t stop_initiator;
    pid_t worker1_pid;
    pid_t worker2_pid;
    int ack_received_from_w1;
    int ack_received_from_w2;
} StationState;

typedef struct {
    int total_tourists;
    int total_rides;
    int vip_served;
    int children_served;
    int bikers;
    int walkers;
    int tk1_sold;
    int tk2_sold;
    int tk3_sold;
    int daily_sold;
    int single_sold;
    int with_guardian;
} DailyStats;

typedef struct {
    int ticket_id;
    int tourist_type;
    int age;
    int is_vip;
    int ticket_type;
    time_t purchase_time;
    time_t first_use;
    time_t expiry_time;
    int rides_count;
    int rides_limit;
    int is_valid;
    int has_guardian;
    int guardian_id;
    float price_paid;
    int is_biker;
} TicketRegistry;

typedef struct {
    int ticket_id;
    time_t usage_time;
    int gate_id;
    char action[32];
} TicketUsageLog;

typedef struct {
    long mtype;
    int command;
    pid_t sender;
    pid_t recipient;
    char message[128];
    int chair_id;
    int tourist_id;
} WorkerMessage;

// zmienne globalne
char log_filename[] = "kolej_log.txt";
FILE *log_file;
int shm_tickets_id = -1;
int shm_state_id = -1;
int shm_stats_id = -1;
int shm_usage_log_id = -1;
int sem_log_id = -1;
int sem_platform_id = -1;
int sem_station_id = -1;  // 1 semafor
int sem_entry_id = -1;    // 4 semafory
int sem_exit_id = -1;
int sem_chair_load_id = -1;
int sem_state_mutex_id = -1;
int msg_queue_id = -1;

TicketRegistry *tickets;
StationState *state;
DailyStats *stats;
TicketUsageLog *usage_logs;
static int usage_log_index = 0;

// Czyszczenie IPC

void cleanup_ipc(void) {
    // pamiec wspoldzielona
    if (state != (void *)-1) shmdt(state);
    if (stats != (void *)-1) shmdt(stats);
    if (tickets != (void *)-1) shmdt(tickets);
    if (usage_logs != (void *)-1) shmdt(usage_logs);
    // shm
    if (shm_state_id != -1) shmctl(shm_state_id, IPC_RMID, NULL);
    if (shm_stats_id != -1) shmctl(shm_stats_id, IPC_RMID, NULL);
    if (shm_tickets_id != -1) shmctl(shm_tickets_id, IPC_RMID, NULL);
    if (shm_usage_log_id != -1) shmctl(shm_usage_log_id, IPC_RMID, NULL);
    // semafory
    if (sem_log_id != -1) semctl(sem_log_id, 0, IPC_RMID);
    if (sem_station_id != -1) semctl(sem_station_id, 0, IPC_RMID);
    if (sem_entry_id != -1) semctl(sem_entry_id, 0, IPC_RMID);
    if (sem_platform_id != -1) semctl(sem_platform_id, 0, IPC_RMID);
    if (sem_exit_id != -1) semctl(sem_exit_id, 0, IPC_RMID);
    if (sem_chair_load_id != -1) semctl(sem_chair_load_id, 0, IPC_RMID);
    if (sem_state_mutex_id != -1) semctl(sem_state_mutex_id, 0, IPC_RMID);
    // kolejka komunikatow
    if (msg_queue_id != -1) msgctl(msg_queue_id, IPC_RMID, NULL);
}

// Semafory

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

void sem_wait(int sem_id, int sem_num) {
    struct sembuf sb = {sem_num, -1, 0};
    while (semop(sem_id, &sb, 1) == -1) {
        if (errno != EINTR) {
            perror("semop wait");
            break;
        }
    }
}

void sem_signal(int sem_id, int sem_num) {
    struct sembuf sb = {sem_num, 1, 0};
    if (semop(sem_id, &sb, 1) == -1) {
        perror("semop signal");
    }
}

// Funkcja do Logów
void log_event(const char *format, ...) {
    if(sem_log_id == -1) return;

    sem_wait(sem_log_id, 0);

    va_list args;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[10];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    FILE *f = fopen(log_filename, "a");
    if (f) {
        fprintf(f, "[%s] ", timestamp);
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    } else {
        perror("fopen log_event");
    }

    sem_signal(sem_log_id, 0);
}

//walidowanie biletu
int validate_ticket(int ticket_id) {
    if (ticket_id < 1 || ticket_id > NUM_TOURISTS) {
        log_event("ERROR: Nieprawidłowy ticket_id=%d", ticket_id);
        return 0;
    }
    
    if (!tickets[ticket_id].is_valid) {
        log_event("ERROR: Bilet %d nieważny", ticket_id);
        return 0;
    }
    
    time_t now = time(NULL);
    if (tickets[ticket_id].expiry_time > 0 && now > tickets[ticket_id].expiry_time) {
        log_event("ERROR: Bilet %d wygasł", ticket_id);
        tickets[ticket_id].is_valid = 0;
        return 0;
    }
    if (tickets[ticket_id].rides_limit > 0 &&
        tickets[ticket_id].rides_count >= tickets[ticket_id].rides_limit) {
        log_event("ERROR: Bilet %d osiągnął limit przejazdów", ticket_id);
        tickets[ticket_id].is_valid = 0;
        return 0;
    }
    
    return 1;
}

// obliczanie ceny biletu
float calculate_ticket_price(int ticket_type, int age) {
    float prices[] = {20.0f, 50.0f, 80.0f, 120.0f, 150.0f};
    float price = prices[ticket_type];
    if (age < 10 || age > 65) price *= 0.75f;
    return price;
}

// rejestracja użycia biletu
void register_ticket_usage(int ticket_id, int gate_id, const char *action) {
    sem_wait(sem_log_id, 0);
    if (usage_log_index < NUM_TOURISTS * 10) {
        usage_logs[usage_log_index].ticket_id = ticket_id;
        usage_logs[usage_log_index].usage_time = time(NULL);
        usage_logs[usage_log_index].gate_id = gate_id;
        strncpy(usage_logs[usage_log_index].action, action, 31);
        usage_logs[usage_log_index].action[31] = '\0';
        usage_log_index++;
    }
    sem_signal(sem_log_id, 0);
}

// znalezienie opiekuna dla dziecka
int find_guardian(int child_id, int child_age) {
    if (child_age >= 8) return -1;
    for (int i = 1; i <= NUM_TOURISTS; i++) {
        if (i == child_id) continue;
        if (tickets[i].age < 18) continue;
        int children_count = 0;
        for (int j = 1; j <= NUM_TOURISTS; j++) {
            if (tickets[j].guardian_id == i) {
                children_count++;
            }
        }
        if (children_count < 2) {
            return i;
        }
    }
    return -1;
}

//Kasjer
void kasjer_process(void) {
    srand(time(NULL) ^ getpid());
    log_event("KASJER: Kasa otwarta PID=%d", getpid());
    
    int tickets_sold = 0;
    float total_revenue = 0.0f;
    WorkerMessage msg;

    while (state->is_running && !state->stop_selling) {
        if (msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long), MSG_TYPE_KASJER, IPC_NOWAIT) == -1) {
            if (errno == ENOMSG) {
                continue;
            }
            if (errno == EINTR) continue;
            if (errno == EIDRM) break;
            break;

        }
        
        if (state->emergency_stop) {
            log_event("KASJER: Awaria, Wstrzymanie pracy");
            while (state->emergency_stop && state->is_running) {
                pause();
            }
            log_event("KASJER: Wznowienie pracy");
        }
        
        if (msg.command == 100) { // BUY_REQUEST
            int tourist_id = msg.sender;
            if (tickets_sold < NUM_TOURISTS && tourist_id >= 1 && tourist_id <= NUM_TOURISTS) {
                tickets_sold++;
                int age = 5 + rand() % 70;
                int ticket_type = rand() % 5;
                int is_vip = (rand() % 100 < 1) ? 1 : 0;
                int is_biker = rand() % 4;
                float price = calculate_ticket_price(ticket_type, age);
                total_revenue += price;
                tickets[tourist_id].ticket_id = tourist_id;
                tickets[tourist_id].age = age;
                tickets[tourist_id].ticket_type = ticket_type;
                tickets[tourist_id].is_vip = is_vip;
                tickets[tourist_id].is_biker = is_biker;
                tickets[tourist_id].purchase_time = time(NULL);
                tickets[tourist_id].first_use = 0;
                tickets[tourist_id].is_valid = 1;
                tickets[tourist_id].price_paid = price;
                tickets[tourist_id].rides_count = 0;
                tickets[tourist_id].has_guardian = 0;
                tickets[tourist_id].guardian_id = -1;
                if (age < 8) {
                    int guardian_id = find_guardian(tourist_id, age);
                    if (guardian_id != -1) {
                        tickets[tourist_id].has_guardian = 1;
                        tickets[tourist_id].guardian_id = guardian_id;
                        sem_wait(sem_log_id, 0);
                        stats->with_guardian++;
                        sem_signal(sem_log_id, 0);
                    }
                }
                switch (ticket_type) {
                    case TICKET_SINGLE:
                        tickets[tourist_id].rides_limit = 1;
                        tickets[tourist_id].expiry_time = 0;
                        sem_wait(sem_log_id, 0);
                        stats->single_sold++;
                        sem_signal(sem_log_id, 0);
                        break;

                    case TICKET_TK1:
                        tickets[tourist_id].rides_limit = 3;
                        tickets[tourist_id].expiry_time = time(NULL) + 3600;
                        sem_wait(sem_log_id, 0);
                        stats->tk1_sold++;
                        sem_signal(sem_log_id, 0);
                        break;

                    case TICKET_TK2:
                        tickets[tourist_id].rides_limit = 5;
                        tickets[tourist_id].expiry_time = time(NULL) + 7200;
                        sem_wait(sem_log_id, 0);
                        stats->tk2_sold++;
                        sem_signal(sem_log_id, 0);
                        break;

                    case TICKET_TK3:
                        tickets[tourist_id].rides_limit = 10;
                        tickets[tourist_id].expiry_time = time(NULL) + 14400;
                        sem_wait(sem_log_id, 0);
                        stats->tk3_sold++;
                        sem_signal(sem_log_id, 0);
                        break;

                    case TICKET_DAILY:
                        tickets[tourist_id].rides_limit = 0;
                        tickets[tourist_id].expiry_time = state->end_time;
                        sem_wait(sem_log_id, 0);
                        stats->daily_sold++;
                        sem_signal(sem_log_id, 0);
                        break;

                }
                const char *ticket_names[] = {"Jednorazowy", "TK1", "TK2", "TK3", "Dzienny"};
                const char *vip_str = is_vip ? "VIP" : "";
                log_event("KASJER: Sprzedano bilet #%d dla Turysty-%d (%s, %d lat) %s",
                    tickets_sold, tourist_id, ticket_names[ticket_type], age, vip_str);
                WorkerMessage reply;
                reply.mtype = msg.recipient;
                reply.command = 3;
                reply.sender = getpid();
                strcpy(reply.message, "BILET GOTOWY");
                msgsnd(msg_queue_id, &reply, sizeof(reply) - sizeof(long), 0);
            }
        }
    }
    log_event("KASJER: Zamkniecie. %d biletow za %.1fPLN", tickets_sold, total_revenue);
    exit(0);
}

// Pracownik na dole kolejki
void dolny_pracownik_process(void) {
    log_event("PRACOWNIK1: start (PID=%d)", getpid());
    
    state->worker1_pid = getpid();
    WorkerMessage msg;
    int chair_count = 0;
    
    while (state->is_running) {
        // Odbiór komunikatów
        if (msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long), MSG_TYPE_WORKER_CTRL, IPC_NOWAIT) != -1) {
            if (msg.command == 1) {
                log_event("PRACOWNIK1: Otrzymano sygnał STOP");
                WorkerMessage ack;
                ack.mtype = MSG_TYPE_WORKER_ACK;
                ack.command = 3; // ACK
                ack.sender = getpid();
                ack.recipient = msg.sender;
                strcpy(ack.message, "ACK STOP W1");
                if (msgsnd(msg_queue_id, &ack, sizeof(ack) - sizeof(long), 0) == -1) {
                    perror("msgsnd ACK");
                }

                // Czekanie na wznowienie
                while (state->emergency_stop && state->is_running) {
                    pause();
                }
            } else if (msg.command == 2) {
                log_event("PRACOWNIK1: Otrzymano sygnał START");
            }
        }
        
        // Wysyłanie krzesła
        if (!state->emergency_stop && state->is_running) {
            sem_wait(sem_state_mutex_id, 0);
            int can_load = (state->busy_chairs < MAX_BUSY_CHAIRS);
            sem_signal(sem_state_mutex_id, 0);

            if (can_load) {
                chair_count++;
                sem_wait(sem_state_mutex_id, 0);
                state->busy_chairs++;
                int current_busy = state->busy_chairs;
                sem_signal(sem_state_mutex_id, 0);

                // Sygnał CHAIR_READY do P2
                WorkerMessage ready_msg;
                ready_msg.mtype = MSG_TYPE_CHAIR_READY;
                ready_msg.command = 4;
                ready_msg.sender = getpid();
                ready_msg.chair_id = chair_count;
                sprintf(ready_msg.message, "Krzesełko %d Gotowe", chair_count);
                msgsnd(msg_queue_id, &ready_msg, sizeof(ready_msg) - sizeof(long), 0);

                // Otwórz bramkę peronu
                int platform_gate = chair_count % PLATFORM_GATES;
                sem_signal(sem_platform_id, platform_gate);
                log_event("PRACOWNIK1: Podstawiono krzesełko #%d (busy=%d), otwarto bramkę %d",chair_count, current_busy, platform_gate);
                // Czekaj na załadunek
                sem_wait(sem_chair_load_id, 0);
                log_event("PRACOWNIK1: Krzesełko #%d załadowane, ruszam", chair_count);
            }
        }
    }
    
    log_event("PRACOWNIK1: koniec pracy");
    exit(0);
}

// Pracownik na górze kolejki
void gorny_pracownik_process(void) {
    log_event("PRACOWNIK2: start (PID=%d)", getpid());
    state->worker2_pid = getpid();
    WorkerMessage msg_in;
    int chairs_received = 0;

    while (state->is_running) {
        // Odczytaj komunikaty
        if (msgrcv(msg_queue_id, &msg_in, sizeof(msg_in) - sizeof(long), MSG_TYPE_WORKER_CTRL, IPC_NOWAIT) != -1) {
            if (msg_in.command == 1) {
                log_event("PRACOWNIK2: Otrzymano sygnał STOP");

                WorkerMessage ack;
                ack.mtype = MSG_TYPE_WORKER_ACK;
                ack.command = 3; // ACK
                ack.sender = getpid();
                ack.recipient = msg_in.sender;
                strcpy(ack.message, "ACK STOP W2");

                if (msgsnd(msg_queue_id, &ack, sizeof(ack) - sizeof(long), 0) == -1) {
                    perror("msgsnd ACK");
                }

                while (state->emergency_stop && state->is_running) {
                    pause();
                }
            } else if (msg_in.command == 2) {
                log_event("PRACOWNIK2: Otrzymano sygnał START");
            }
        }
        
        // Rozładunek krzesełek
        if (!state->emergency_stop && state->is_running) {
            if (msgrcv(msg_queue_id, &msg_in, sizeof(msg_in) - sizeof(long),
                MSG_TYPE_CHAIR_READY, IPC_NOWAIT) != -1) {
                if (msg_in.command == 4) { // CHAIR_READY
                    chairs_received++;
                    log_event("PRACOWNIK2: Odebrano krzesełko #%d, rozładunek", msg_in.chair_id);
                    sem_wait(sem_state_mutex_id, 0);
                    state->busy_chairs--;
                    int current_busy = state->busy_chairs;
                    sem_signal(sem_state_mutex_id, 0);
                    log_event("PRACOWNIK2: Krzesełko #%d rozładowane (busy=%d)",msg_in.chair_id, current_busy);
                }
            }
        }
    }
    
    log_event("PRACOWNIK2: koniec pracy");
    exit(0);
}

void turysta_process(int tourist_id) {
    srand(time(NULL) ^ getpid() ^ tourist_id);

    // Zakup biletu
    WorkerMessage req;
    req.mtype = MSG_TYPE_KASJER;
    req.sender = tourist_id;
    req.recipient = MSG_TYPE_TOURIST_BASE + tourist_id;
    req.command = 100;
    strcpy(req.message, "POPROSZE BILET");
    if (msgsnd(msg_queue_id, &req, sizeof(req) - sizeof(long), 0) == -1) {
        perror("msgsnd buy");
        exit(1);
    }

    // Czekanie na bilet 
    WorkerMessage resp;
    int got_ticket = 0;
    time_t wait_start = time(NULL);
    while (!got_ticket && (time(NULL) - wait_start) < 10) {
        if (msgrcv(msg_queue_id, &resp, sizeof(resp) - sizeof(long),
            MSG_TYPE_TOURIST_BASE + tourist_id, IPC_NOWAIT) != -1) {
            got_ticket = 1;
            break;
        }
    }
    if (!got_ticket) {
        log_event("TURYSTA-%d: Timeout oczekiwania na bilet!", tourist_id);
        exit(0);
    }

    //pobieranie danych biletu
    int age = tickets[tourist_id].age;
    int is_vip = tickets[tourist_id].is_vip;
    int is_biker = tickets[tourist_id].is_biker;
    int has_guardian = tickets[tourist_id].has_guardian;
    int guardian_id = tickets[tourist_id].guardian_id;
     
    
    char type_str[64];
    if (is_biker == 0) strcpy(type_str, "Pieszy");
    else sprintf(type_str, "Rowerzysta (trasa T%d)", is_biker);
    log_event("TURYSTA-%d: Ma bilet: %s, %d lat, VIP=%d, Opiekun=%d",tourist_id, type_str, age, is_vip, guardian_id);
    
    // Walidacja biletu

    if (!validate_ticket(tourist_id)) {
        log_event("TURYSTA-%d: Bilet nieważny!", tourist_id);
        exit(0);
    }
    
    

    if (age < 8 && !has_guardian) {
        log_event("TURYSTA-%d: BŁĄD - Dziecko <8 lat bez opiekuna! Odrzucono.", tourist_id);
        exit(0);
    }

    if (age < 8 && has_guardian) {
        log_event("TURYSTA-%d: Czekam na opiekuna (ID=%d)", tourist_id, guardian_id);
        time_t wait = time(NULL);
        while ((time(NULL) - wait) < 10) {
            if (tickets[guardian_id].is_valid) break;
        }
    }

    // wejscie

    if (!is_vip) {
        int gate = rand() % ENTRY_GATES;
        sem_wait(sem_entry_id, gate);
        register_ticket_usage(tourist_id, gate, "WEJSCIE_BRAMKA");
        sem_wait(sem_station_id, 0);
        sem_wait(sem_state_mutex_id, 0);
        state->people_in_station++;
        int in_station = state->people_in_station;
        sem_signal(sem_state_mutex_id, 0);
        log_event("TURYSTA-%d: Weszło przez bramkę %d (w stacji: %d)",
            tourist_id, gate, in_station);
        sem_signal(sem_entry_id, gate);
    } else {
        log_event("TURYSTA-%d: VIP - omija kolejkę!", tourist_id);
        sem_wait(sem_log_id, 0);
        stats->vip_served++;
        sem_signal(sem_log_id, 0);
    }

    // Bramki peronu (3 bramki)

    int platform_gate = tourist_id % PLATFORM_GATES;
    sem_wait(sem_platform_id, platform_gate);
    sem_wait(sem_state_mutex_id, 0);
    state->people_on_platform++;
    int on_platform = state->people_on_platform;
    sem_signal(sem_state_mutex_id, 0);
    register_ticket_usage(tourist_id, platform_gate, "WEJSCIE_PERON");
    log_event("TURYSTA-%d: Wchodzi na krzesełko! (na peronie: %d)", tourist_id, on_platform);

    if (!is_vip) {
        sem_wait(sem_state_mutex_id, 0);
        if (state->people_in_station > 0) {
            state->people_in_station--;
        }
        sem_signal(sem_state_mutex_id, 0);
        sem_signal(sem_station_id, 0);
    }

    // Załadunek
    tickets[tourist_id].rides_count++;
    if (tickets[tourist_id].first_use == 0) {
        tickets[tourist_id].first_use = time(NULL);
    }

    // Sygnał do P1: załadowano
    sem_signal(sem_chair_load_id, 0);

    // Wyjście z wyciągu
    int exit_route = rand() % EXIT_ROUTES;
    sem_wait(sem_exit_id, exit_route);
    register_ticket_usage(tourist_id, exit_route, "WYJSCIE_GORA");
    sem_signal(sem_exit_id, exit_route);
    sem_wait(sem_state_mutex_id, 0);
    if (state->people_on_platform > 0) {
        state->people_on_platform--;
    }
    sem_signal(sem_state_mutex_id, 0);

    // Wyjście z górnej stacji (2 drogi)

    if (is_biker == 0) {
        sem_wait(sem_log_id, 0);
        stats->walkers++;
        sem_signal(sem_log_id, 0);
        int walk_time = rand() % 3 + 2;
        register_ticket_usage(tourist_id, 0, "TRASA_SPACER");
        log_event("TURYSTA-%d: Spacer trasą (%ds)", tourist_id, walk_time);
    } else {
        sem_wait(sem_log_id, 0);
        stats->bikers++;
        sem_signal(sem_log_id, 0);
        int trail_time = (is_biker == 1) ? TRAIL_T1 : (is_biker == 2) ? TRAIL_T2 : TRAIL_T3;
        char trail_name[32];
        sprintf(trail_name, "TRASA_T%d", is_biker);
        register_ticket_usage(tourist_id, is_biker, trail_name);
        log_event("TURYSTA-%d: Rower trasą T%d (%ds)", tourist_id, is_biker, trail_time);
    }

    sem_wait(sem_log_id, 0);
    stats->total_rides++;
    sem_signal(sem_log_id, 0);

    if (age < 10) {
        sem_wait(sem_log_id, 0);
        stats->children_served++;
        sem_signal(sem_log_id, 0);
    }

    log_event("TURYSTA-%d: koniec trasy.", tourist_id);
    exit(0);
}


// obsługa awaryjnego zatrzymania
void emergency_stop_handler(int sig) {
    if (!state->emergency_stop) {
        log_event("*** ZATRZYMANIE AWARYJNE (SIGUSR1) PID=%d ***", getpid());
        state->emergency_stop = 1;
        state->stop_initiator = getpid();
        state->ack_received_from_w1 = 0;
        state->ack_received_from_w2 = 0;

        WorkerMessage msg;
        msg.mtype = MSG_TYPE_WORKER_CTRL;
        msg.command = 1; // STOP
        msg.sender = getpid();
        strcpy(msg.message, "AWARYJNY STOP");

        msgsnd(msg_queue_id, &msg, sizeof(msg) - sizeof(long), 0);
        log_event("MAIN: Czekam na ACK od pracowników...");
        time_t wait_start = time(NULL);
        WorkerMessage ack;

        while ((!state->ack_received_from_w1 || !state->ack_received_from_w2) && (time(NULL) - wait_start) < 5) {
            if (msgrcv(msg_queue_id, &ack, sizeof(ack) - sizeof(long),
                MSG_TYPE_WORKER_ACK, IPC_NOWAIT) != -1) {
                if (ack.command == 3) { // ACK
                    if (ack.sender == state->worker1_pid) {
                        state->ack_received_from_w1 = 1;
                        log_event("MAIN: ACK otrzymany od PRACOWNIK1");
                    } else if (ack.sender == state->worker2_pid) {
                        state->ack_received_from_w2 = 1;
                        log_event("MAIN: ACK otrzymany od PRACOWNIK2");
                    }
                }
            }
        }

        if (state->ack_received_from_w1 && state->ack_received_from_w2) {
            log_event("MAIN: Oba ACK otrzymane - system zatrzymany");
        } else {
            log_event("MAIN: OSTRZEŻENIE - Nie wszystkie ACK otrzymane w czasie");
        }
    }
}

// obsługa wznowienia
void resume_handler(int sig) {
    if (state->emergency_stop) {
        log_event("*** WZNOWIENIE (SIGUSR2) PID=%d ***", getpid());
        
        WorkerMessage msg;
        msg.mtype = MSG_TYPE_WORKER_CTRL;
        msg.command = 2; // START
        msg.sender = getpid();
        strcpy(msg.message, "WZNOWIENIE");
        
        msgsnd(msg_queue_id, &msg, sizeof(msg) - sizeof(long), 0);
        
        state->emergency_stop = 0;
        state->ack_received_from_w1 = 0;
        state->ack_received_from_w2 = 0;
        log_event("MAIN: System wznowiony");
    }
}

void sigchld_handler(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

//main
int main(void) {

    srand(time(NULL));
    
    // Logi

    log_file = fopen(log_filename, "w");
    if (!log_file) { 
        perror("fopen log");
        return 1;
    }
    fclose(log_file);
    printf("===============================\n");
    printf("SYMULACJA KOLEI LINOWEJ\n");
    printf("===============================\n");
    printf("Plik z logami: %s\n\n", log_filename);

    // SHM

    key_t key_state = ftok(".", 'S');
    key_t key_stats = ftok(".", 'T');
    key_t key_tickets = ftok(".", 'K');
    key_t key_usage = ftok(".", 'U');


     if (key_state == -1 || key_stats == -1 || key_tickets == -1 || key_usage == -1) {
        perror("ftok");
        return 1;
    }

    shm_state_id = shmget(key_state, sizeof(StationState), IPC_CREAT | 0600);
    shm_stats_id = shmget(key_stats, sizeof(DailyStats),   IPC_CREAT | 0600);
    shm_tickets_id = shmget(key_tickets, sizeof(TicketRegistry) * (NUM_TOURISTS + 1), IPC_CREAT | 0600);
    shm_usage_log_id = shmget(key_usage, sizeof(TicketUsageLog) * (NUM_TOURISTS * 10), IPC_CREAT | 0600);

    if (shm_state_id == -1 || shm_stats_id == -1 || shm_tickets_id == -1 || shm_usage_log_id == -1) {
        perror("Error: shmget");
        cleanup_ipc();
        return 1;
    }

    state = (StationState *)shmat(shm_state_id, NULL, 0);
    stats = (DailyStats *)shmat(shm_stats_id,  NULL, 0);
    tickets = (TicketRegistry *)shmat(shm_tickets_id, NULL, 0);
    usage_logs = (TicketUsageLog *)shmat(shm_usage_log_id, NULL, 0);

    if (state == (void *)-1 || stats == (void *)-1 || tickets == (void *)-1 || usage_logs == (void *)-1) {
        perror("shmat");
        cleanup_ipc();
        return 1;
    }

    memset(state, 0, sizeof(StationState));
    memset(stats, 0, sizeof(DailyStats));
    memset(tickets, 0, sizeof(TicketRegistry) * (NUM_TOURISTS + 1));
    memset(usage_logs, 0, sizeof(TicketUsageLog) * (NUM_TOURISTS * 10));

    // inicjalizacja semaforów
    union semun arg;

    sem_log_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (sem_log_id == -1) {
        perror("semget log");
        cleanup_ipc();
        return 1;
    }

    arg.val = 1;
    if (semctl(sem_log_id, 0, SETVAL, arg) == -1) {
        perror("semctl log");
        cleanup_ipc();
        return 1;
    }

    sem_station_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (sem_station_id == -1) {
        perror("semget station");
        cleanup_ipc();
        return 1;
    }
    arg.val = MAX_STATION_CAPACITY;
    if (semctl(sem_station_id, 0, SETVAL, arg) == -1) {
        perror("semctl station");
        cleanup_ipc();
        return 1;
    }

    // Semafor bramek wejściowych (4 bramki)
    sem_entry_id = semget(IPC_PRIVATE, ENTRY_GATES, IPC_CREAT | 0600);
    if (sem_entry_id == -1) {
        perror("semget entry");
        cleanup_ipc();
        return 1;
    }
    for (int i = 0; i < ENTRY_GATES; i++) {
        arg.val = 1;
        if (semctl(sem_entry_id, i, SETVAL, arg) == -1) {
            perror("semctl entry");
            cleanup_ipc();
            return 1;
        }
    }

    // Semafory bramek peronu (3 bramki)

    sem_platform_id = semget(IPC_PRIVATE, PLATFORM_GATES, IPC_CREAT | 0600);
    if (sem_platform_id == -1) {
        perror("semget platform");
        cleanup_ipc();
        return 1;
    }

    for (int i = 0; i < PLATFORM_GATES; i++) {
        arg.val = 0;
        if (semctl(sem_platform_id, i, SETVAL, arg) == -1) {
            perror("semctl platform");
            cleanup_ipc();
            return 1;
        }
    }

    // Semafory wyjść z górnej stacji (2)

    sem_exit_id = semget(IPC_PRIVATE, EXIT_ROUTES, IPC_CREAT | 0600);
    if (sem_exit_id == -1) {
        perror("semget exit");
        cleanup_ipc();
        return 1;
    }

    for (int i = 0; i < EXIT_ROUTES; i++) {
        arg.val = 1;
        if (semctl(sem_exit_id, i, SETVAL, arg) == -1) {
            perror("semctl exit");
            cleanup_ipc();
            return 1;
        }
    }

    sem_chair_load_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (sem_chair_load_id == -1) { perror("semget chair_load"); cleanup_ipc(); return 1; }
    arg.val = 0; semctl(sem_chair_load_id, 0, SETVAL, arg);
    sem_state_mutex_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (sem_state_mutex_id == -1) { perror("semget state_mutex"); cleanup_ipc(); return 1; }
    arg.val = 1; semctl(sem_state_mutex_id, 0, SETVAL, arg);

    // Kolejka komunikatów

    key_t key_msg = ftok(".", 'Q');
    if (key_msg == -1) {
        perror("ftok msg");
        cleanup_ipc();
        return 1;
    }

    msg_queue_id = msgget(key_msg, IPC_CREAT | 0600);
    if (msg_queue_id == -1) {
        perror("msgget");
        cleanup_ipc();
        return 1;
    }

    log_event("SYSTEM: IPC zainicjalizowane");
    printf("Shared memory utworzone\n");
    printf("Semafory utworzone (log, stacja, bramki: 4+3, wyjścia: 2)\n");
    printf("Kolejka komunikatów ID: %d\n\n utworzona", msg_queue_id);

    // Obsługa sygnałów

    struct sigaction sa_stop, sa_resume;

    sa_stop.sa_handler = emergency_stop_handler;
    sigemptyset(&sa_stop.sa_mask);
    sa_stop.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa_stop, NULL) == -1) {
        perror("sigaction SIGUSR1");
    }

    sa_resume.sa_handler = resume_handler;
    sigemptyset(&sa_resume.sa_mask);
    sa_resume.sa_flags = 0;
    if (sigaction(SIGUSR2, &sa_resume, NULL) == -1) {
        perror("sigaction SIGUSR2");
    }

    log_event("SYSTEM: Handlery SIGUSR1/2 skonfigurowane");
    //printf("Sygnaly: kill -USR1 %d (STOP), kill -USR2 %d (RESUME)\n", getpid(), getpid());

    // Inicjalizacja stanu
    
    state->is_running = 1;
    state->start_time = time(NULL);
    state->end_time = state->start_time + SIM_DURATION;

    // tworzenie procesów
    // proces kasjera
    pid_t kasjer_pid = fork();
    if (kasjer_pid == 0) {
        kasjer_process();
        exit(0);
    } 
    else if (kasjer_pid > 0) {
        printf("Kasjer PID: %d\n", kasjer_pid);
        log_event("Kasjer uruchomiony PID=%d", kasjer_pid);
    } 
    else {
        perror("fork kasjer");
        cleanup_ipc();
        return 1;
    }

    //proces pracownika na dole kolejki
    pid_t pid_pracownik1 = fork();
    if (pid_pracownik1 == 0) {
        dolny_pracownik_process();
        exit(0);
    } else if (pid_pracownik1 > 0) {
        printf("Pracownik1 PID: %d\n", pid_pracownik1);
        log_event("PRACOWNIK1 uruchomiony PID=%d", pid_pracownik1);
    } else {
        perror("fork pracownik1");
        cleanup_ipc();
        return 1;
    }

    //process pracownika na górze kolejki
    pid_t pid_pracownik2 = fork();
    if (pid_pracownik2 == 0) {
        gorny_pracownik_process();
        exit(0);
    } else if (pid_pracownik2 > 0) {
        printf("Pracownik2 PID: %d\n", pid_pracownik2);
        log_event("PRACOWNIK2 uruchomiony PID=%d", pid_pracownik2);
    } else {
        perror("fork pracownik2");
        cleanup_ipc();
        return 1;
    }

    //proces turysty
    pid_t tourist_pids[NUM_TOURISTS];

    for (int i = 1; i <= NUM_TOURISTS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            turysta_process(i);
            exit(0);
        } else if (pid > 0) {
            tourist_pids[i-1] = pid;
            log_event("TURYSTA-%d: fork PID=%d", i, pid);
        } else {
            perror("fork turysta");
            cleanup_ipc();
            return 1;
        }
    }
    printf("Procesy turystów uruchomione (%d turystów)\n\n", NUM_TOURISTS);

    // Log początkowy
    // char time_str[32];
    // strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&state->start_time));
    // log_event("==========================================");
    // log_event("Rozpoczęcie Symulacji Kolei Linowej");
    // log_event("Czas rozpoczecia: %s", time_str);
    // log_event("Krzeslka: %d", TOTAL_CHAIRS);
    // log_event("Bramki wejsciowe: %d, bramki peronu: %d", ENTRY_GATES, PLATFORM_GATES);
    // log_event("Czas trwania: %d sekund", SIM_DURATION);
    // log_event("===========================================");
    
    // printf("Start: %s | Czas: %ds\n", time_str, SIM_DURATION);
    // printf("Krzesełka: %d/%d | Turyści: %d\n\n", MAX_BUSY_CHAIRS, TOTAL_CHAIRS, NUM_TOURISTS); 

   // Główna pętla symulacji
   
    log_event("START: Główna pętla symulacji %ds", SIM_DURATION);
    time_t start_loop = time(NULL);
    int test_done = 0;

    while (time(NULL) - start_loop < SIM_DURATION) {
        int elapsed = time(NULL) - start_loop;
        int remaining = SIM_DURATION - elapsed;

        if (!test_done && elapsed >= 15 && elapsed < 20) {
            test_done = 1;
            log_event("*** AUTO TEST AWARII ***");
            kill(getpid(), SIGUSR1);
            sleep(5);
            kill(getpid(), SIGUSR2);
        }

        // Status
        if (remaining % 10 == 0) {
            int total_sold = stats->single_sold + stats->tk1_sold + stats->tk2_sold + stats->tk3_sold + stats->daily_sold;
            printf("Pozostało: %ds | Sprzedano: %d | Przejazdy: %d | Busy: %02d\r", remaining, total_sold, stats->total_rides, state->busy_chairs);
            fflush(stdout);
        }
        sleep(1);
    }

    printf("\n");

    // Koniec symulacji
    log_event("SYSTEM: Koniec symulacji, zamykanie...");
    printf("Koniec symulacji.\n");

    state->stop_selling = 1;
    sleep(2);
    state->is_running = 0;

    //Czyszczenie procesów

    if (kasjer_pid > 0) {  
        log_event("Zamykanie kasjera PID=%d", kasjer_pid);
        kill(kasjer_pid, SIGTERM);
        waitpid(kasjer_pid, NULL, 0);
        printf("Kasjer zakończony\n");
    }

    if (pid_pracownik1 > 0) {
        log_event("Zamykanie pracownika na dole PID=%d", pid_pracownik1);
        kill(pid_pracownik1, SIGTERM);
        waitpid(pid_pracownik1, NULL, 0);
        printf("PRACOWNIK1 zakonczony");
    }

    if (pid_pracownik2 > 0) { 
        log_event("Zamykanie pracownik na gorze PID=%d", pid_pracownik2);
        kill(pid_pracownik2, SIGTERM);
        waitpid(pid_pracownik2, NULL, 0);
        printf("PRACOWNIK2 zakonczony\n");
    }

    for (int i = 0; i < NUM_TOURISTS; i++) {
        if (tourist_pids[i] > 0) {
            kill(tourist_pids[i], SIGTERM);
            waitpid(tourist_pids[i],NULL, WNOHANG);
            log_event("TURYSTA-%d zakonczony (PID=%d)", i+1, tourist_pids[i]);
        }
    }

    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0);

    // Podsumowanie
    int total_tickets = stats->single_sold + stats->tk1_sold + stats->tk2_sold + stats->tk3_sold + stats->daily_sold;

    log_event("==================================");
    log_event("RAPORT KOŃCOWY:");
    log_event("__________________________________");
    log_event("BILETY:");
    log_event("Bilety sprzedane: %d/%d", total_tickets, NUM_TOURISTS);
    log_event("  - Jednorazowe: %d", stats->single_sold);
    log_event("  - TK1: %d", stats->tk1_sold);
    log_event("  - TK2: %d", stats->tk2_sold);
    log_event("  - TK3: %d", stats->tk3_sold);
    log_event("  - Dzienne: %d", stats->daily_sold);
    log_event("__________________________________");
    log_event("PRZEJAZDY:");
    log_event("ogółem: %d", stats->total_rides);
    log_event("Piesi: %d | Rowerzyści: %d", stats->walkers, stats->bikers);
    log_event("__________________________________");
    log_event("OBSŁUŻENI:");
    log_event("VIP obsłużeni: %d", stats->vip_served);
    log_event("Dzieci z opiekunem: %d", stats->with_guardian);
    log_event("Dzieci (<10 lat): %d", stats->children_served);
    log_event("Rowerzyści: %d", stats->bikers);
    log_event("Piesi: %d", stats->walkers);
    printf("==================================");

    printf("\n==================================\n");
    printf("RAPORT KOŃCOWY:\n");
    printf("__________________________________\n");
    printf("BILETY:\n");
    printf("Bilety sprzedane: %d/%d\n", total_tickets, NUM_TOURISTS);
    printf("  - Jednorazowe: %d\n", stats->single_sold);
    printf("  - TK1: %d\n", stats->tk1_sold);
    printf("  - TK2: %d\n", stats->tk2_sold);
    printf("  - TK3: %d\n", stats->tk3_sold);
    printf("  - Dzienne: %d\n", stats->daily_sold);
    printf("__________________________________\n");
    printf("PRZEJAZDY:\n");
    printf("ogółem: %d\n", stats->total_rides);
    printf("Piesi: %d | Rowerzyści: %d\n", stats->walkers, stats->bikers);
    printf("__________________________________\n");
    printf("OBSŁUŻENI:\n");
    printf("VIP obsłużeni: %d\n", stats->vip_served);
    printf("Dzieci z opiekunem: %d\n", stats->with_guardian);
    printf("Dzieci (<10 lat): %d\n", stats->children_served);
    printf("Rowerzyści: %d\n", stats->bikers);
    printf("Piesi: %d\n", stats->walkers);
    printf("==================================\n");
    cleanup_ipc();

    return 0;
}
