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

char log_filename[] = "kolej_log.txt";
FILE *log_file;


int shm_tickets_id; 
int shm_state_id;
int shm_stats_id;

int sem_log_id;
int sem_station_id;  // 1 semafor
int sem_entry_id;    // 4 semafory

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
    if (age < 10 || age > 65) price *= 0.75f;  // 25% zniżki
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

    // Semafor pojemności stacji (1 semafor, MAX_STATION_CAPACITY)
    sem_station_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (sem_station_id == -1) {
        perror("semget station");
        cleanup_ipc();
        return 1;
    }
    arg.val = MAX_STATION_CAPACITY;  // 50 osób max
    semctl(sem_station_id, 0, SETVAL, arg);

    // Semafor bramek wejściowych (4 bramki)
    sem_entry_id = semget(IPC_PRIVATE, ENTRY_GATES, IPC_CREAT | 0600);
    if (sem_entry_id == -1) {
        perror("semget entry");
        cleanup_ipc();
        return 1;
    }
    for (int i = 0; i < ENTRY_GATES; i++) {
        arg.val = 1;  // Każda bramka dostępna
        semctl(sem_entry_id, i, SETVAL, arg);
    }

    // Tworzenie procesu kasjera

    pid_t kasjer_pid = fork();
    if (kasjer_pid == 0) {
        kasjer_process();
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


    // incjializacja ogólna

    memset(state, 0, sizeof(StationState));
    memset(stats,  0, sizeof(DailyStats));
    
    state->is_running = 1;
    state->start_time = time(NULL);
    state->end_time = state->start_time + SIM_DURATION;

    char time_str[9];
    strftime(time_str, sizeof(time_str), "%T", localtime(&state->start_time));
    log_event("------------------------------------------");
    log_event("Czas rozpoczecia: %s", time_str);
    log_event("Liczba krzeslek: %d", TOTAL_CHAIRS);
    log_event("------------------------------------------");
    
    printf("Start time: %s\n", time_str);
    printf("czas trwania: %d sekund\n", SIM_DURATION);

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
            printf("Status: Stacja %d/50 | Passes %d\r", 
                state->people_in_station, state->total_passes);
            fflush(stdout);
        }
    }

    //sleep(SIM_DURATION);
    log_event("KONIEC petli symulacji");
    printf("Koniec.\n");

    if (kasjer_pid > 0) {
        state->stop_selling = 1;  // Sygnał do zamknięcia kasy
        sleep(2);  // Czas na sprzedaż ostatniego biletu
        log_event("Zamykanie kasjera PID=%d", kasjer_pid);
        kill(kasjer_pid, SIGTERM);
        int status;
        waitpid(kasjer_pid, &status, 0);
        log_event("Kasjer zakonczony");
    }


    cleanup_ipc();

    return 0;
}
