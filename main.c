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

#define TOTAL_CHAIRS 72
#define NUM_TOURISTS 30
#define SIM_DURATION 60

#define MAX_STATION_CAPACITY 50
#define ENTRY_GATES 4 

typedef struct {
    int is_running;
    int busy_chairs;
    int people_in_station;
    int stop_selling;
    time_t start_time;
    time_t end_time;
    int emergency_stop;
    int total_passes;
} StationState;

typedef struct {
    int total_tourists;
    int total_rides;
    int vip_served;
    int children_served;
    int bikers;
    int walkers;
} DailyStats;

typedef struct {
    int ticket_id;
    int tourist_type; 
    int age;
    int is_vip;
    int ticket_type;
    time_t first_use;
    int rides_count;
} TicketRegistry;

typedef struct {
    long mtype;         
    int command;        // 1: STOP, 2: START, 3: ACK  
    pid_t sender;       
    char message[128]; 
} WorkerMessage;

char log_filename[] = "kolej_log.txt";
FILE *log_file;


int shm_tickets_id; 
int shm_state_id;
int shm_stats_id;

int sem_log_id;
int sem_station_id;  // 1 semafor
int sem_entry_id;    // 4 semafory

int msg_queue_id;

TicketRegistry *tickets;
StationState *state;
DailyStats  *stats;


// Czyszczenie IPC

void cleanup_ipc(void) {
    // pamiec wspoldzielona
    if (state != (void *)-1)  shmdt(state);
    if (stats != (void *)-1)  shmdt(stats);
    if (tickets != (void *)-1) shmdt(tickets);

    if (shm_state_id != -1)   shmctl(shm_state_id, IPC_RMID, NULL);
    if (shm_stats_id != -1)   shmctl(shm_stats_id, IPC_RMID, NULL);
    if (shm_tickets_id != -1) shmctl(shm_tickets_id, IPC_RMID, NULL);

    //semafory
    if (sem_log_id != -1) semctl(sem_log_id, 0, IPC_RMID);
    if (sem_station_id != -1) semctl(sem_station_id, 0, IPC_RMID);
    if (sem_entry_id != -1) semctl(sem_entry_id, 0, IPC_RMID);

    // kolejka komunikatow

    if (msg_queue_id != -1) {
        msgctl(msg_queue_id, IPC_RMID, NULL);
    }

}

// Semafory

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

void sem_wait(int sem_id, int sem_num) {
    struct sembuf sb = {sem_num, -1, 0};
    if (semop(sem_id, &sb, 1) == -1) {
        perror("semop wait");
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
    sem_wait(sem_log_id, 0);

    va_list args;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[10];
    strcpy(timestamp, ctime(&now)); 
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    
    FILE *f = fopen(log_filename, "a");
    if (f) {
        fprintf(f, "[%s] ", timestamp);
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }

    sem_signal(sem_log_id, 0);
}

// obliczanie ceny biletu
float calculate_ticket_price(int ticket_type, int age) {
    float prices[] = {20.0f, 50.0f, 80.0f, 120.0f, 150.0f};
    float price = prices[ticket_type];
    if (age < 10 || age > 65) price *= 0.75f;
    return price;
}

//Kasjer
void kasjer_process(void) {
    log_event("KASJER: Kasa otwarta PID=%d", getpid());
    
    int tickets_sold = 0;
    float total_revenue = 0.0f;
    
    while (state->is_running && !state->stop_selling) {
        sleep(rand() % 3 + 1);
        
        if (state->emergency_stop) {
            log_event("KASJER: Awaria, Wstrzymanie pracy");
            while (state->emergency_stop && state->is_running) {
                sleep(1);
            }
            log_event("KASJER: Wznowienie pracy");
        }
        
        if (rand() % 2) {
            tickets_sold++;
            int age = 5 + rand() % 70;
            int ticket_type = rand() % 5;
            float price = calculate_ticket_price(ticket_type, age);
            total_revenue += price;
            
            // SHM
            tickets[tickets_sold].ticket_id = tickets_sold;
            tickets[tickets_sold].age = age;
            tickets[tickets_sold].ticket_type = ticket_type;
            tickets[tickets_sold].first_use = time(NULL);
            
            log_event("KASJER: Bilet#%d typ%d wiek%d=%.1fPLN (sprzedanych: %d)", 
                      tickets_sold, ticket_type, age, price, tickets_sold);
        }
    }
    
    log_event("KASJER: Zamkniecie. %d biletow na %.1fPLN", tickets_sold, total_revenue);
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
                // Czekanie na wznowienie
                while (state->emergency_stop && state->is_running) {
                    sleep(1);
                }
            } else if (msg.command == 2) {
                log_event("PRACOWNIK1: Otrzymano sygnał wznowienia od PID=%d", msg.sender);
            }
        }
        
        // Wysyłanie krzesła
        if (!state->emergency_stop && state->is_running) {
            chair_count++;
            state->busy_chairs++;
            log_event("PRACOWNIK1: Wysłano krzesełko #%d (zajęte: %d/36)", 
                     chair_count, state->busy_chairs);
            sleep(3);
        } else {
            sleep(1);
        }
    }
    
    log_event("PRACOWNIK1: koniec pracy");
}

// Pracownik na górze kolejki
void gorny_pracownik_process(void) {
    log_event("PRACOWNIK2: start (PID=%d)", getpid());
    
    WorkerMessage msg_in, msg_out;
    int tourist_leave = 0;
    
    while (state->is_running) {
        // Odczytaj komunikaty
        if (msgrcv(msg_queue_id, &msg_in, sizeof(msg_in) - sizeof(long), 2, IPC_NOWAIT) != -1) {
            if (msg_in.command == 1) {
                log_event("PRACOWNIK2: Otrzymano STOP od PID=%d: %s", msg_in.sender, msg_in.message);
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
            log_event("PRACOWNIK2: Turysta#%d opuścił (zajęte: %d/36)", 
                      tourist_leave, state->busy_chairs);
            // Potwierdzenie dla dolnego pracownika
            msg_out.mtype = 1;
            msg_out.command = 3;  // ACK
            msg_out.sender = getpid();
            sprintf(msg_out.message, "ACK turysta#%d", tourist_leave);
            msgsnd(msg_queue_id, &msg_out, sizeof(msg_out) - sizeof(long), 0);
            
            sleep(5);
        } else {
            sleep(1);
        }
    }
    
    log_event("PRACOWNIK2: koniec pracy");
}

void turysta_process(int tourist_id) {
    log_event("TURYSTA-%d: start PID=%d", tourist_id, getpid());
    
    int is_biker = rand() % 4;  // 0=spacer, 1=T1, 2=T2, 3=T3
    int age = 5 + rand() % 70;  // 5-75 lat
    int ticket_id = tourist_id;
    
    int ticket_type = tickets[ticket_id].ticket_type;
    float price = calculate_ticket_price(ticket_type, age);
    
    char type_str[32];
    if (is_biker == 0) strcpy(type_str, "Pieszy");
    else sprintf(type_str, "Rowerzysta (trasa %d)", is_biker);
    
    log_event("TURYSTA-%d: %s, wiek %d, bilet typ %d (%.0f PLN)", 
              tourist_id, type_str, age, ticket_type, price);
    
    sleep(rand() % 3 + 1);
    
    // wejscie

    int gate = rand() % ENTRY_GATES;
    sem_wait(sem_entry_id, gate); // Bramka

    sem_wait(sem_station_id, 0); // Pojemność 
    state->people_in_station++;
    log_event("TURYSTA-%d: przez bramkę #%d → stacja (%d/50)", tourist_id, gate+1, state->people_in_station);
    sem_signal(sem_entry_id, gate);   // zwolnienie bramki
    

    while (state->is_running && state->busy_chairs >= 36) {
        log_event("TURYSTA-%d: Oczekuje na krzesełko (zajęte: %d/36)...", tourist_id, state->busy_chairs);
        sleep(2);
    }
    if (!state->is_running) {
        log_event("TURYSTA-%d: koniec - brak krzeseł", tourist_id);
        exit(0);
    }

    tickets[ticket_id].rides_count++;
    log_event("TURYSTA-%d: Wsiadł na krzesełko (przejazd #%d)", tourist_id, tickets[ticket_id].rides_count);

    sem_signal(sem_station_id, 0);


    log_event("TURYSTA-%d: Przejazd w górę (4s)...", tourist_id);
    sleep(4);
    log_event("TURYSTA-%d: Dotarł na górę", tourist_id);

    if (is_biker == 0) {
        stats->walkers++;
        log_event("TURYSTA-%d: Pieszy - wejście na szlak pieszy (walkers=%d)",
                tourist_id, stats->walkers);
    } else {
        stats->bikers++;
        log_event("TURYSTA-%d: Rowerzysta - trasa #%d (bikers=%d)",
                tourist_id, is_biker, stats->bikers);
    }

    
    if (state->people_in_station > 0) {
        state->people_in_station--;
    }
    log_event("TURYSTA-%d: opuszcza system (na dole, stacja=%d/50)",
            tourist_id, state->people_in_station);

    log_event("TURYSTA-%d: koniec", tourist_id);
    exit(0);
}


// obsługa awaryjnego zatrzymania
void emergency_stop_handler(int sig) {
    if (!state->emergency_stop) {
        log_event("*** ZATRZYMANIE AWARYJNE (SIGUSR1) ***");
        state->emergency_stop = 1;
        
        WorkerMessage msg;
        msg.mtype = 1;
        msg.command = 1;     // STOP
        msg.sender = getpid();
        strcpy(msg.message, "AWARYJNY STOP");
        
        if (msgsnd(msg_queue_id, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
            perror("msgsnd emergency");
        }
    }
}

// obsługa wznowienia
void resume_handler(int sig) {
    if (state->emergency_stop) {
        log_event("*** MAIN: WZNOWIENIE (SIGUSR2) ***");
        
        WorkerMessage msg;
        msg.mtype = 1;
        msg.command = 2;     // PLAY
        msg.sender = getpid();
        strcpy(msg.message, "WZNOWIENIE");
        msgsnd(msg_queue_id, &msg, sizeof(msg) - sizeof(long), 0);
        
        sleep(1);
        state->emergency_stop = 0;
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

    printf("SYMULACJA KOLEI LINOWEJ\n");
    printf("PLIK Z LOGAMI: %s\n\n", log_filename);

    // SHM stanu stacji i statystyk

    key_t key_state = ftok(".", 'S');
    key_t key_stats = ftok(".", 'T');
    if (key_state == -1 || key_stats == -1) {
        perror("ftok");
        return 1;
    }

    shm_state_id = shmget(key_state, sizeof(StationState), IPC_CREAT | 0600);
    shm_stats_id = shmget(key_stats, sizeof(DailyStats),   IPC_CREAT | 0600);
    if (shm_state_id == -1 || shm_stats_id == -1) {
        perror("Error: shmget");
        cleanup_ipc();
        return 1;
    }

    state = (StationState *)shmat(shm_state_id, NULL, 0);
    stats = (DailyStats  *)shmat(shm_stats_id,  NULL, 0);
    if (state == (void *)-1 || stats == (void *)-1) {
        perror("Error: shmat");
        cleanup_ipc();
        return 1;
    }

    // SHM rejestru biletów

    key_t key_tickets = ftok(".", 'K');
    if (key_tickets == -1) {
        perror("ftok tickets");
        cleanup_ipc();
        return 1;
    }

    shm_tickets_id = shmget(key_tickets, sizeof(TicketRegistry) * (NUM_TOURISTS + 1), 
                            IPC_CREAT | 0600);
    if (shm_tickets_id == -1) {
        perror("shmget tickets");
        cleanup_ipc();
        return 1;
    }

    tickets = (TicketRegistry *)shmat(shm_tickets_id, NULL, 0);
    if (tickets == (void *)-1) {
        perror("shmat tickets");
        cleanup_ipc();
        return 1;
    }

    memset(tickets, 0, sizeof(TicketRegistry) * (NUM_TOURISTS + 1));


    // inicjalizacja semafora logów
    union semun arg;

    sem_log_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (sem_log_id == -1) {
        perror("Error: semget");
        cleanup_ipc();
        return 1;
    }

    
    arg.val = 1;
    if (semctl(sem_log_id, 0, SETVAL, arg) == -1) {
        perror("Error: semctl");
        cleanup_ipc();
        return 1;
    }

    log_event("-- Semafory zainicjalizowane --");

    // inicjalizacja semaforów stacji i wejść

    // Semafor pojemności stacji
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
    arg.val = MAX_STATION_CAPACITY;  // 50 osób max
    semctl(sem_station_id, 0, SETVAL, arg);

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
        semctl(sem_entry_id, i, SETVAL, arg);
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

    log_event("SYSTEM: Kolejka komunikatow utworzona (key=0x%X)", key_msg);
    printf("Msg queue ID: %d\n", msg_queue_id);

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
    printf("Test sygnalow: kill -USR1 %d (STOP), kill -USR2 %d (RESUME)\n", getpid(), getpid());

    // Inicjalizacja stanu

    memset(state, 0, sizeof(StationState));
    memset(stats,  0, sizeof(DailyStats));
    
    state->is_running = 1;
    state->start_time = time(NULL);
    state->end_time = state->start_time + SIM_DURATION;

    // Log początkowy
    char time_str[9];
    strftime(time_str, sizeof(time_str), "%T", localtime(&state->start_time));
    log_event("------------------------------------------");
    log_event("Czas rozpoczecia: %s", time_str);
    log_event("Liczba krzeslek: %d", TOTAL_CHAIRS);
    log_event("------------------------------------------");
    
    printf("Start time: %s\n", time_str);
    printf("czas trwania: %d sekund\n", SIM_DURATION);

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

    pid_t pid_turysta1 = fork();
    if (pid_turysta1 == 0) {
        turysta_process(1);
        exit(0);
    } else if (pid_turysta1 > 0) {
        printf("Turysta1 PID: %d\n", pid_turysta1);
        log_event("TURYSTA-1: fork PID=%d", pid_turysta1);
    } else {
        perror("fork turysta1");
        cleanup_ipc();
        return 1;
    }



    // Początek symulacji

    // printf("Krzesełka: %d, turyści: %d\n", TOTAL_CHAIRS, NUM_TOURISTS);
    // printf("czas trwania: %d sekund\n", SIM_DURATION);

   // Główna pętla symulacji
   
    log_event("START: Glowna petla symulacji %ds", SIM_DURATION);
    time_t start_loop = time(NULL);

    while (time(NULL) - start_loop < SIM_DURATION) {
        sleep(5);
        
        // Losowy ruch turystów
        int arrivals = rand() % 6;
        
        for (int i = 0; i < arrivals && state->people_in_station < MAX_STATION_CAPACITY; i++) {
            int gate = rand() % ENTRY_GATES;
            
            // Wejście przez bramkę
            sem_wait(sem_entry_id, gate);
            state->total_passes++;
            log_event("WEJSCIE bramka%d (#%d)", gate+1, state->total_passes);
            sem_signal(sem_entry_id, gate);
            
            // Na stację
            sem_wait(sem_station_id, 0);
            state->people_in_station++;
            sem_signal(sem_station_id, 0);
        }
        
        // Status
        if ((time(NULL) - start_loop) % 15 < 5) {
            log_event("STATUS: Stacja %d/50 | Przejsc %d | Krzeselka %d/36", 
                    state->people_in_station, 
                    state->total_passes, 
                    state->busy_chairs);
            printf("Status: Stacja %d/50 | Przejscia %d\r", 
                state->people_in_station, state->total_passes);
            fflush(stdout);
        }
    }

    // //sleep(SIM_DURATION);
    // log_event("KONIEC petli symulacji");
    printf("Koniec.\n");

    state->stop_selling = 1;
    sleep(1);
    state->is_running = 0;
    sleep(1);

    int status;
    if (kasjer_pid > 0) {
        sleep(2);  
        log_event("Zamykanie kasjera PID=%d", kasjer_pid);
        kill(kasjer_pid, SIGTERM);
        waitpid(kasjer_pid, &status, 0);
        log_event("Kasjer zakonczony");
    }

    if (pid_pracownik1 > 0) {
        log_event("Zamykanie pracownika na dole PID=%d", pid_pracownik1);
        kill(pid_pracownik1, SIGTERM);
        waitpid(pid_pracownik1, &status, 0);
        log_event("PRACOWNIK1 zakonczony");
    }

    if (pid_pracownik2 > 0) { 
        log_event("Zamykanie pracownik na gorze PID=%d", pid_pracownik2);
        kill(pid_pracownik2, SIGTERM);
        waitpid(pid_pracownik2, &status, 0);
        log_event("PRACOWNIK2 zakonczony");
    }

    // ---

    if (pid_turysta1 > 0) {
        log_event("Oczekiwanie na turysta1 PID=%d", pid_turysta1);
        kill(pid_pracownik2, SIGTERM);
        waitpid(pid_turysta1, &status, 0);
        log_event("TURYSTA-1 zakonczony");
    }


    cleanup_ipc();

    return 0;
}
