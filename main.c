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
#define SIM_DURATION 60
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
    
    WorkerMessage msg;
    int chair_count = 0;
    
    while (state->is_running) {
        // Odbiór komunikatów
        if (msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long), 1, IPC_NOWAIT) != -1) {
            if (msg.command == 1) {
                log_event("PRACOWNIK1: Otrzymano sygnał zatrzymania od PID=%d", msg.sender);

                WorkerMessage ack;
                ack.mtype = 2;
                ack.command = 3; // ACK
                ack.sender = getpid();
                ack.recipient = msg.sender;
                strcpy(ack.message, "ACK STOP");
                if (msgsnd(msg_queue_id, &ack, sizeof(ack) - sizeof(long), 0) == -1) {
                    perror("msgsnd ACK");
                }

                // Czekanie na wznowienie
                while (state->emergency_stop && state->is_running) {
                    sleep(1);
                }
            } else if (msg.command == 2) {
                log_event("PRACOWNIK1: Otrzymano sygnał START od PID=%d", msg.sender);
            }
        }
        
        // Wysyłanie krzesła
        if (!state->emergency_stop && state->is_running && state->busy_chairs < MAX_BUSY_CHAIRS) {
            chair_count++;
            state->busy_chairs++;
            log_event("PRACOWNIK1: Wysłano krzesełko #%d (zajęte: %d/%d)", 
                     chair_count, state->busy_chairs, MAX_BUSY_CHAIRS);
            sleep(3);
        } else {
            sleep(1);
        }
    }
    
    log_event("PRACOWNIK1: koniec pracy");
    exit(0);
}

// Pracownik na górze kolejki
void gorny_pracownik_process(void) {
    log_event("PRACOWNIK2: start (PID=%d)", getpid());
    
    WorkerMessage msg_in;
    int tourist_leave = 0;
    
    while (state->is_running) {
        // Odczytaj komunikaty
        if (msgrcv(msg_queue_id, &msg_in, sizeof(msg_in) - sizeof(long), 2, IPC_NOWAIT) != -1) {
            if (msg_in.command == 1) {
                log_event("PRACOWNIK2: Otrzymano STOP od PID=%d: %s", msg_in.sender, msg_in.message);

                WorkerMessage ack;
                ack.mtype = 1;
                ack.command = 3; // ACK
                ack.sender = getpid();
                ack.recipient = msg_in.sender;
                strcpy(ack.message, "ACK STOP");

                if (msgsnd(msg_queue_id, &ack, sizeof(ack) - sizeof(long), 0) == -1) {
                    perror("msgsnd ACK");
                }

                while (state->emergency_stop && state->is_running) {
                    sleep(1);
                }
            } else if (msg_in.command == 2) {
                log_event("PRACOWNIK2: Otrzymano RESUME od PID=%d", msg_in.sender);
            }
        }
        
        // wyslanie turysty
        if (!state->emergency_stop && state->busy_chairs > 0 && state->is_running) {
            tourist_leave++;
            state->busy_chairs--;
            log_event("PRACOWNIK2: Turysta#%d opuścił (zajęte: %d/%d)", 
                      tourist_leave, state->busy_chairs, MAX_BUSY_CHAIRS);
            
            sleep(5);
        } else {
            sleep(1);
        }
    }
    
    log_event("PRACOWNIK2: koniec pracy");
    exit(0);
}

void turysta_process(int tourist_id) {
    srand(time(NULL) ^ getpid());
    log_event("TURYSTA-%d: start PID=%d", tourist_id, getpid());
    
    // 0=spacer, 1=T1, 2=T2, 3=T3
    int is_biker = rand() % 4;  
    int age = tickets[tourist_id].age;  // 5-75 lat
    int ticket_type = tickets[tourist_id].ticket_type;
    int is_vip = tickets[tourist_id].is_vip;
     
    
    char type_str[64];
    if (is_biker == 0) strcpy(type_str, "Pieszy");
    else sprintf(type_str, "Rowerzysta (trasa %d)", is_biker);
    

    const char *ticket_names[] = {"Jednorazowy", "TK1", "TK2", "TK3", "Dzienny"};
    log_event("TURYSTA-%d: %s, wiek %d, bilet typ %s%s", tourist_id, type_str, age, ticket_names[ticket_type], is_vip ? " VIP" : "");
    

    sleep(rand() % 2 + 1);
    
    // Walidacja biletu

    if (!validate_ticket(tourist_id)) {
        log_event("TURYSTA-%d: Bilet nieważny, koniec", tourist_id);
        exit(0);
    }
    
    // wejscie

    int gate = rand() % ENTRY_GATES;

    if (is_vip) {
        log_event("TURYSTA-%d: VIP - priorytet na bramce #%d", tourist_id, gate+1);
    }

    sem_wait(sem_entry_id, gate); // Bramka
    register_ticket_usage(tourist_id, gate, "WEJSCIE_STACJA");

    sem_wait(sem_station_id, 0); // Pojemność 
    state->people_in_station++;
    state->total_passes++;

    log_event("TURYSTA-%d: przez bramkę #%d → stacja (%d/%d)", tourist_id, gate+1, state->people_in_station, MAX_STATION_CAPACITY);
    sem_signal(sem_entry_id, gate);   // zwolnienie bramki
    

    // Bramki peronu (3 bramki)

    int platform_gate = rand() % PLATFORM_GATES;
    sem_wait(sem_platform_id, platform_gate);
    state->people_on_platform++;
    register_ticket_usage(tourist_id, platform_gate, "WEJSCIE_PERON");
    log_event("TURYSTA-%d: Wejście na peron przez bramkę #%d (%d osób na peronie)", tourist_id, platform_gate+1, state->people_on_platform);
    sem_signal(sem_platform_id, platform_gate);

    // Zwolnienie pojemności stacji

    if (state->people_in_station > 0) {
        state->people_in_station--;
    }

    // oczekiwanie na krzesełko
    while (state->is_running && state->busy_chairs >= MAX_BUSY_CHAIRS) {
        log_event("TURYSTA-%d: Oczekuje na krzesełko (zajęte: %d/%d)...", tourist_id, state->busy_chairs, MAX_BUSY_CHAIRS);
        sleep(2);
    }

    if (!state->is_running) {
        log_event("TURYSTA-%d: koniec - brak krzeseł", tourist_id);
        exit(0);
    }

    // Wejście na krzesełko

    tickets[tourist_id].rides_count++;
    if (tickets[tourist_id].first_use == 0) {
        tickets[tourist_id].first_use = time(NULL);
    }

    log_event("TURYSTA-%d: Wsiadł na krzesełko (przejazd #%d/%d)", 
              tourist_id, tickets[tourist_id].rides_count,
              tickets[tourist_id].rides_limit > 0 ? tickets[tourist_id].rides_limit : 999);

    if (state->people_on_platform > 0) {
        state->people_on_platform--;
    }

    // jazda w góre

    log_event("TURYSTA-%d: Przejazd w górę ...", tourist_id);
    sleep(4);
    log_event("TURYSTA-%d: Dotarł na górę", tourist_id);

    // Wyjście z górnej stacji (2 drogi)

    int exit_route = rand() % EXIT_ROUTES;
    sem_wait(sem_exit_id, exit_route);
    log_event("TURYSTA-%d: Wyjście drogą #%d", tourist_id, exit_route+1);
    sem_signal(sem_exit_id, exit_route);

    if (is_biker == 0) {
        stats->walkers++;
        log_event("TURYSTA-%d: Pieszy - wejście na szlak pieszy (walkers=%d)", tourist_id, stats->walkers);
            sleep(rand() % 3 + 2);

    } else {
        stats->bikers++;
        int trail_time = (is_biker == 1) ? TRAIL_T1 : (is_biker == 2) ? TRAIL_T2 : TRAIL_T3;
        log_event("TURYSTA-%d: Rowerzysta - trasa T%d (bikers=%d)", tourist_id, is_biker, stats->bikers);
        sleep(trail_time);
    }

    stats->total_rides++;
    if (is_vip) stats->vip_served++;
    if (age < 10) stats->children_served++;

    log_event("TURYSTA-%d: koniec", tourist_id);
    exit(0);
}


// obsługa awaryjnego zatrzymania
void emergency_stop_handler(int sig) {
    if (!state->emergency_stop) {
        log_event("*** ZATRZYMANIE AWARYJNE (SIGUSR1) PID=%d ***", getpid());
        state->emergency_stop = 1;
        state->stop_initiator = getpid();
        state->waiting_for_ack = 1;


        WorkerMessage msg1, msg2;
        msg1.mtype = 1;
        msg1.command = 1;
        msg1.sender = getpid();
        strcpy(msg1.message, "AWARYJNY STOP");

        msg2.mtype = 2;
        msg2.command = 1;
        msg2.sender = getpid();
        strcpy(msg2.message, "AWARYJNY STOP");
        
        if (msgsnd(msg_queue_id, &msg1, sizeof(msg1) - sizeof(long), 0) == -1) {
            perror("msgsnd emergency stop 1");
        }
        if (msgsnd(msg_queue_id, &msg2, sizeof(msg2) - sizeof(long), 0) == -1) {
            perror("msgsnd emergency stop 2");
        }

        log_event("MAIN: Czekanie na ACK od pracowników...");
    }
}

// obsługa wznowienia
void resume_handler(int sig) {
    if (state->emergency_stop) {
        log_event("*** WZNOWIENIE (SIGUSR2) PID=%d ***", getpid());
        
        WorkerMessage msg1, msg2;

        msg1.mtype = 1;
        msg1.command = 2;
        msg1.sender = getpid();
        strcpy(msg1.message, "WZNOWIENIE");

        msg2.mtype = 2;
        msg2.command = 2;
        msg2.sender = getpid();

        strcpy(msg2.message, "WZNOWIENIE");
        
        if (msgsnd(msg_queue_id, &msg1, sizeof(msg1) - sizeof(long), 0) == -1) {
            perror("msgsnd resume 1");
        }

        if (msgsnd(msg_queue_id, &msg2, sizeof(msg2) - sizeof(long), 0) == -1) {
            perror("msgsnd resume 2");
        }
        
        sleep(1);
        state->emergency_stop = 0;
        state->waiting_for_ack = 0;
    }
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
    stats = (DailyStats  *)shmat(shm_stats_id,  NULL, 0);
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

    // Semafor pojemnosci
    key_t key_station = ftok(".", 'P');
    if (key_station == -1) {
        perror("ftok station");
        cleanup_ipc();
        return 1;
    }
    sem_station_id = semget(key_station, 1, IPC_CREAT | 0600);
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
    key_t key_entry = ftok(".", 'E');
    if (key_entry == -1) {
        perror("ftok entry");
        cleanup_ipc();
        return 1;
    }
    sem_entry_id = semget(key_entry, ENTRY_GATES, IPC_CREAT | 0600);
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

    key_t key_platform = ftok(".", 'L');
    if (key_platform == -1) {
        perror("ftok platform");
        cleanup_ipc();
        return 1;
    }

    sem_platform_id = semget(key_platform, PLATFORM_GATES, IPC_CREAT | 0600);
    if (sem_platform_id == -1) {
        perror("semget platform");
        cleanup_ipc();
        return 1;
    }

    for (int i = 0; i < PLATFORM_GATES; i++) {
        arg.val = 1;
        if (semctl(sem_platform_id, i, SETVAL, arg) == -1) {
            perror("semctl platform");
            cleanup_ipc();
            return 1;
        }
    }

    // Semafory wyjść z górnej stacji (2)

    key_t key_exit = ftok(".", 'X');
    if (key_exit == -1) {
        perror("ftok exit");
        cleanup_ipc();
        return 1;
    }

    sem_exit_id = semget(key_exit, EXIT_ROUTES, IPC_CREAT | 0600);
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

    // Obsługa sygnału awaryjnego zatrzymania

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
    printf("Sygnaly: kill -USR1 %d (STOP), kill -USR2 %d (RESUME)\n", getpid(), getpid());

    // Inicjalizacja stanu
    
    state->is_running = 1;
    state->start_time = time(NULL);
    state->end_time = state->start_time + SIM_DURATION;

    // Log początkowy
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&state->start_time));
    log_event("==========================================");
    log_event("Rozpoczęcie Symulacji Kolei Linowej");
    log_event("Czas rozpoczecia: %s", time_str);
    log_event("Krzeslka:: %d", TOTAL_CHAIRS);
    log_event("Bramki wejsciowe: %d, bramki peronu: %d", ENTRY_GATES, PLATFORM_GATES);
    log_event("Czas trwania: %d sekund", SIM_DURATION);
    log_event("===========================================");
    
    printf("Start: %s | Czas: %ds\n", time_str, SIM_DURATION);
    printf("Krzesełka: %d/%d | Turyści: %d\n\n", MAX_BUSY_CHAIRS, TOTAL_CHAIRS, NUM_TOURISTS);
    // Tworzenie procesów 
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

    log_event("***EMERGENCY TEST: Uruchamiam za 10s ***");
    sleep(10);

    if (state->is_running) {
        log_event("*** TEST STOP: SIGUSR1 ***");
        printf("\n*** TEST EMERGENCY STOP ***\n");
        kill(getpid(), SIGUSR1);
        sleep(6);
        
        log_event("*** TEST RESUME: SIGUSR2 ***");
        printf("*** TEST RESUME ***\n");
        kill(getpid(), SIGUSR2);
        
        log_event("*** AUTO-TEST ZAKOŃCZONY ***");
    }

    printf("Kontynuacja symulacji...\n");
    printf("Manual test: kill -USR1 %d (STOP) | kill -USR2 %d (RESUME)\n\n", getpid(), getpid());

   // Główna pętla symulacji
   
    log_event("START: Główna pętla symulacji %ds", SIM_DURATION);
    time_t start_loop = time(NULL);

    while (time(NULL) - start_loop < SIM_DURATION) {
        sleep(5);
        
        int elapsed = time(NULL) - start_loop;
        int remaining = SIM_DURATION - elapsed;

        if (remaining > 0 && remaining <= 10) {
            printf("\033[91m %2ds do zamknięcia! \033[0m\r", remaining);
            fflush(stdout);
            log_event("COUNTDOWN: %ds!", remaining);
        }

        // Status
        if ((time(NULL) - start_loop) % 15 < 5) {
            log_event("STATUS: Stacja %d/%d | Peron %d | Przejść %d | Krzesełka %d/%d", 
                state->people_in_station, MAX_STATION_CAPACITY,
                state->people_on_platform,
                state->total_passes,
                state->busy_chairs, 
                MAX_BUSY_CHAIRS);

            printf("Status: Stacja %d/%d | Peron %d | Krzesełka %d/%d | Przejazdy %d\r",
                state->people_in_station, MAX_STATION_CAPACITY,
                state->people_on_platform,
                state->busy_chairs, 
                MAX_BUSY_CHAIRS,
                stats->total_rides);

            fflush(stdout);
        }
    }

    // //sleep(SIM_DURATION);
    log_event("Koniec symulacji");
    printf("Koniec symulacji.\n");

    state->stop_selling = 1;
    state->is_running = 0;
    sleep(3);

    int status;
    if (kasjer_pid > 0) {
        sleep(2);  
        log_event("Zamykanie kasjera PID=%d", kasjer_pid);
        kill(kasjer_pid, SIGTERM);
        waitpid(kasjer_pid, &status, 0);
        log_event("Kasjer zakończony");
        printf("Kasjer zakończony\n");
    }

    if (pid_pracownik1 > 0) {
        log_event("Zamykanie pracownika na dole PID=%d", pid_pracownik1);
        kill(pid_pracownik1, SIGTERM);
        waitpid(pid_pracownik1, &status, 0);
        log_event("PRACOWNIK1 zakonczony");
        printf("PRACOWNIK1 zakonczony");
    }

    if (pid_pracownik2 > 0) { 
        log_event("Zamykanie pracownik na gorze PID=%d", pid_pracownik2);
        kill(pid_pracownik2, SIGTERM);
        waitpid(pid_pracownik2, &status, 0);
        log_event("PRACOWNIK2 zakonczony");
        printf("PRACOWNIK2 zakonczony\n");
    }

    for (int i = 0; i < NUM_TOURISTS; i++) {
        if (tourist_pids[i] > 0) {
            kill(tourist_pids[i], SIGTERM);
            waitpid(tourist_pids[i], &status, 0);
            log_event("TURYSTA-%d zakonczony (PID=%d)", i+1, tourist_pids[i]);
        }
    }

    // Podsumowanie
    int total_tickets_sold = 0;
    int total_rides = 0;
    float total_revenue = 0.0f;
    
    for (int i = 1; i <= NUM_TOURISTS; i++) {
        if (tickets[i].ticket_id > 0) {
            total_tickets_sold++;
            total_revenue += tickets[i].price_paid;
        }
        total_rides += tickets[i].rides_count;
    }

    log_event("==================================");
    log_event("RAPORT KOŃCOWY:");
    log_event("__________________________________");
    log_event("BILETY:");
    log_event("Bilety sprzedane: %d/%d", total_tickets_sold, NUM_TOURISTS);
    log_event("Jednorazowe: %d | TK1: %d | TK2: %d | TK3: %d | Dzienne: %d",stats->single_sold, stats->tk1_sold, stats->tk2_sold, stats->tk3_sold, stats->daily_sold);
    log_event("__________________________________");
    log_event("PRZEJAZDY:");
    log_event("Ogółem: %d (średnio %.1f/turysta)", total_rides, total_tickets_sold > 0 ? (float)total_rides / total_tickets_sold : 0);
    log_event("Piesi: %d | Rowerzyści: %d", stats->walkers, stats->bikers);
    log_event("__________________________________");
    log_event("OBSŁUGA:");
    log_event("  VIP: %d | Dzieci (<10lat): %d", stats->vip_served, stats->children_served);
    log_event("  Przejść przez bramki: %d", state->total_passes);
    log_event("__________________________________");
    log_event("INNE:");
    log_event("Max osób na stacji: %d/%d", state->people_in_station, MAX_STATION_CAPACITY);
    log_event("Max krzesełek zajętych: %d/%d", state->busy_chairs, MAX_BUSY_CHAIRS);
    log_event("═══════════════════════════════════════════════");

    cleanup_ipc();

    return 0;
}
