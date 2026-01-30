#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <pthread.h>
#include "struktury.h"
#include "operacje.h"
#include "logger.h"

// Globalne zmienne
static volatile sig_atomic_t shutdown_flag = 0;
static volatile sig_atomic_t interrupt_flag = 0;

static int g_sem_id = -1;
static int g_msg_id = -1;
static int g_msg_worker_id = -1;
static int g_shm_id = -1;
static SharedMemory* g_shm = NULL;

static pid_t cashier_pid = 0;
static pid_t worker1_pid = 0;
static pid_t worker2_pid = 0;

// Lista procesów turystów
#define MAX_TOURIST_PROCESSES 10000
static pid_t tourist_pids[MAX_TOURIST_PROCESSES];
static int tourist_pid_count = 0;
static pthread_mutex_t tourist_mutex = PTHREAD_MUTEX_INITIALIZER;

// Handler sygnałów
void main_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        interrupt_flag = 1;
        shutdown_flag = 1;
    } else if (sig == SIGCHLD) {
        // Obsługiwane w wątku sprzątającym
    }
}

// Wątek sprzątający zakończone procesy turystów
void* reaper_thread(void* arg) {
    (void)arg;
    
    while (!shutdown_flag || tourist_pid_count > 0) {
        int status;
        pid_t finished_pid;
        
        while ((finished_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (finished_pid == worker1_pid || finished_pid == worker2_pid || finished_pid == cashier_pid) {
                continue;
            }
            
            pthread_mutex_lock(&tourist_mutex);
            
            // Usunięcie z listy turystów
            for (int i = 0; i < tourist_pid_count; i++) {
                if (tourist_pids[i] == finished_pid) {
                    tourist_pids[i] = tourist_pids[tourist_pid_count - 1];
                    tourist_pid_count--;
                    break;
                }
            }
            
            pthread_mutex_unlock(&tourist_mutex);
        }
        
    }
    
    return NULL;
}

// Wysyłanie sygnału do wszystkich procesów
void send_signal_to_all(int sig) {
    if (cashier_pid > 0) kill(cashier_pid, sig);
    if (worker1_pid > 0) kill(worker1_pid, sig);
    if (worker2_pid > 0) kill(worker2_pid, sig);
    
    pthread_mutex_lock(&tourist_mutex);
    for (int i = 0; i < tourist_pid_count; i++) {
        if (tourist_pids[i] > 0) {
            kill(tourist_pids[i], sig);
        }
    }
    pthread_mutex_unlock(&tourist_mutex);
}

// Utworzenie procesu turysty
pid_t create_tourist(int tourist_id, int age, TouristType type, bool is_vip, int children_count) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("Błąd fork() przy tworzeniu turysty");
        return -1;
    }

    if (pid == 0) {
        // Proces potomny
        char id_str[16], age_str[16], type_str[16], vip_str[16], children_str[16];
        snprintf(id_str, sizeof(id_str), "%d", tourist_id);
        snprintf(age_str, sizeof(age_str), "%d", age);
        snprintf(type_str, sizeof(type_str), "%d", type);
        snprintf(vip_str, sizeof(vip_str), "%d", is_vip ? 1 : 0);
        snprintf(children_str, sizeof(children_str), "%d", children_count);
        
        execl("./tourist", "tourist", id_str, age_str, type_str, vip_str, children_str, NULL);
        perror("Błąd execl() przy uruchamianiu turysty");
        _exit(1);
    }
    
    return pid;
}

// Inicjalizacja pamięci dzielonej
void init_shared_memory(void) {
    memset(g_shm, 0, sizeof(SharedMemory));
    
    g_shm->is_running = true;
    g_shm->emergency_stop = false;
    g_shm->gates_closed = false;
    g_shm->cashier_open = false;  // Kasa zamknięta na początku
    g_shm->simulation_start = time(NULL);
    g_shm->simulation_end = 0;
    
    // Inicjalizacja krzesełek
    for (int i = 0; i < MAX_CHAIRS; i++) {
        g_shm->chairs[i].id = i;
        g_shm->chairs[i].passenger_count = 0;
        g_shm->chairs[i].cyclist_count = 0;
        g_shm->chairs[i].pedestrian_count = 0;
        g_shm->chairs[i].in_transit = false;
        for (int j = 0; j < CHAIR_CAPACITY; j++) {
            g_shm->chairs[i].passengers[j] = -1;
        }
    }
    
    g_shm->main_pid = getpid();
    g_shm->next_tourist_id = 0;
    g_shm->next_ticket_id = 0;
    g_shm->next_chair_id = 0;
    g_shm->gate_entries_count = 0;
}

int main(void) {
    // Konfiguracja sygnałów
    struct sigaction sa;
    sa.sa_handler = main_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    
    // Ignoruj niektóre sygnały
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    
    srand(time(NULL));
    
    printf("\n");
    printf("============================================================\n");
    printf("        SYMULACJA KOLEI LINOWEJ - START\n");
    printf("============================================================\n");
    printf("Liczba turystów: %d\n", TOTAL_TOURISTS);
    printf("Max osób na stacji: %d\n", STATION_CAPACITY);
    printf("Krzesełka aktywne: %d / %d\n", MAX_ACTIVE_CHAIRS, MAX_CHAIRS);
    printf("============================================================\n\n");
    
    // czyszczenie starych zasobów
    czysc_zasoby();
    logger_clear_files();
    
    // Utworzenie zasobów IPC
    g_sem_id = utworz_semafory();
    g_shm_id = utworz_pamiec();
    g_msg_id = utworz_kolejke();
    g_msg_worker_id = utworz_kolejke_worker();
    
    g_shm = dolacz_pamiec(g_shm_id);
    init_shared_memory();
    
    // Inicjalizacja loggera
    logger_init();
    
    logger(LOG_SYSTEM, "Symulacja rozpoczęta - zasoby IPC utworzone");
    
    // Uruchomienie procesów pracowników
    worker1_pid = fork();
    if (worker1_pid == 0) {
        execl("./worker", "worker", NULL);
        perror("Błąd execl() przy uruchamianiu worker1");
        _exit(1);
    }
    logger(LOG_SYSTEM, "Uruchomiono pracownika 1 (stacja dolna) PID: %d", worker1_pid);
    
    worker2_pid = fork();
    if (worker2_pid == 0) {
        execl("./worker2", "worker2", NULL);
        perror("Błąd execl() przy uruchamianiu worker2");
        _exit(1);
    }
    logger(LOG_SYSTEM, "Uruchomiono pracownika 2 (stacja górna) PID: %d", worker2_pid);
    
    // Uruchom proces kasjera
    cashier_pid = fork();
    if (cashier_pid == 0) {
        execl("./cashier", "cashier", NULL);
        perror("Błąd execl() przy uruchamianiu kasjera");
        _exit(1);
    }
    logger(LOG_SYSTEM, "Uruchomiono kasjera PID: %d", cashier_pid);
    
    // Zapisanie PIDów
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->cashier_pid = cashier_pid;
    g_shm->worker1_pid = worker1_pid;
    g_shm->worker2_pid = worker2_pid;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Uruchomienie wątku sprzątającego
    pthread_t reaper;
    pthread_create(&reaper, NULL, reaper_thread, NULL);
    
    // generowanie turystów
    int tourists_created = 0;
    time_t sim_start = time(NULL);
    
    logger(LOG_SYSTEM, "Rozpoczynam generowanie turystów...");
    
    while (!shutdown_flag) {
        // Sprawdzanie czy nie minął czas pracy
        time_t now = time(NULL);
        if (now - sim_start >= WORK_END_TIME) {
            logger(LOG_SYSTEM, "Osiągnięto czas Tk - zamykam bramki wejściowe");
            
            sem_opusc(g_sem_id, SEM_MAIN);
            g_shm->gates_closed = true;
            sem_podnies(g_sem_id, SEM_MAIN);
            
            break;
        }
        
        // Sprawdzanie czy możemy utworzyć więcej procesów
        if(tourists_created < TOTAL_TOURISTS){
            pthread_mutex_lock(&tourist_mutex);
            int current_count = tourist_pid_count;
            pthread_mutex_unlock(&tourist_mutex);
            
            if (current_count >= MAX_TOURIST_PROCESSES) {
                continue;
            }
            
            // Generowanie turysty
            tourists_created++;
            
            int tourist_id = tourists_created;
            
            // Losowe parametry turysty
            int age;
            int age_roll = rand() % 100;
            if (age_roll < 15) {
                age = 4 + rand() % 6; // 4-9 lat (dzieci)
            } else if (age_roll < 25) {
                age = 10 + rand() % 8; // 10-17 lat (młodzież)
            } else if (age_roll < 85) {
                age = 18 + rand() % 47; // 18-64 lat (dorośli)
            } else {
                age = 65 + rand() % 20; // 65-84 lat (seniorzy)
            }
            
            TouristType type = (rand() % 2) ? TOURIST_CYCLIST : TOURIST_PEDESTRIAN;
            bool is_vip = (rand() % 100) < VIP_PERCENT;
            
            // Dzieci z opiekunem (dorośli 18-64 lat mogą mieć dzieci)
            int children_count = 0;
            if (age >= 18 && age < 65 && rand() % 100 < 10) {
                if (type == TOURIST_CYCLIST) {
                    children_count = 1;  // Rowerzysta może mieć max 1 dziecko
                } else {
                    children_count = 1 + rand() % 2;  // Pieszy może mieć 1-2 dzieci
                }
            }
            
            pid_t pid = create_tourist(tourist_id, age, type, is_vip, children_count);
            
            if (pid > 0) {
                pthread_mutex_lock(&tourist_mutex);
                if (tourist_pid_count < MAX_TOURIST_PROCESSES) {
                    tourist_pids[tourist_pid_count++] = pid;
                }
                pthread_mutex_unlock(&tourist_mutex);
                
                sem_opusc(g_sem_id, SEM_MAIN);
                g_shm->total_tourists_created += 1 + children_count;
                sem_podnies(g_sem_id, SEM_MAIN);

            } else if (pid == -1) {
                logger(LOG_SYSTEM, "Błąd tworzenia turysty #%d", tourist_id);
            }
            
            // Losowe opóźnienie między turystami
            volatile int delay_count = 0;
            int max_delay = rand() % 100000;
            while (delay_count < max_delay) {
                delay_count++;
            }
        }
    }


    //pętla czekająca na zakończenie obchodu wszystkich turystów po końcu czasu
    while(!shutdown_flag) {
        pthread_mutex_lock(&tourist_mutex);
        int remaining = tourist_pid_count;
        pthread_mutex_unlock(&tourist_mutex);
        
        if (remaining == 0) {
            logger(LOG_SYSTEM, "Brak aktywnych turystów");
            break;
        }
        
        // Przerwij jeśli Ctrl+C
        if (interrupt_flag) {
            logger(LOG_SYSTEM, "Przerwanie shutdown (Ctrl+C)");
            break;
        }
        
    }

    // Opóźnienie przed wyłączeniem
    if (!interrupt_flag) {
        logger(LOG_SYSTEM, "Oczekiwanie %d sekund przed wyłączeniem...", SHUTDOWN_DELAY);
        time_t delay_start = time(NULL);
        while ((time(NULL) - delay_start) < SHUTDOWN_DELAY && !interrupt_flag) {
        }
    }
    logger(LOG_SYSTEM, "Zamykanie symulacji...");
    
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->is_running = false;
    g_shm->simulation_end = time(NULL);
    sem_podnies(g_sem_id, SEM_MAIN);

    
    send_signal_to_all(SIGTERM);
    // Czekanie na zakończenie procesów głównych
    int status;
    if (cashier_pid > 0) waitpid(cashier_pid, &status, 0);
    if (worker1_pid > 0) waitpid(worker1_pid, &status, 0);
    if (worker2_pid > 0) waitpid(worker2_pid, &status, 0);

    sem_opusc(g_sem_id, SEM_MAIN);
            int in_station = g_shm->tourists_in_station;
            int on_platform = g_shm->tourists_on_platform;
            int active_chairs = g_shm->active_chairs;
            int at_top = g_shm->tourists_at_top;
            int at_cashier = g_shm->tourists_at_cashier;
            int descending = g_shm->tourists_descending;
    sem_podnies(g_sem_id, SEM_MAIN);
            
    logger(LOG_SYSTEM, "(kasa: %d, stacja: %d, peron: %d, krzesełka: %d, góra: %d, zjazd: %d)", 
                        at_cashier, in_station, on_platform, active_chairs, at_top, descending);

    // Dobicie pozostałych procesów turystów
    int killed_count = 0;
    pthread_mutex_lock(&tourist_mutex);
    for (int i = 0; i < tourist_pid_count; i++) {
        if (tourist_pids[i] > 0) {
            kill(tourist_pids[i], SIGKILL);
            waitpid(tourist_pids[i], &status, 0);
            killed_count++;
        }
    }
    tourist_pid_count = 0;
    pthread_mutex_unlock(&tourist_mutex);
    
    // kończenie wątku sprzątającego
    shutdown_flag = 1;
    pthread_join(reaper, NULL);
    
    if (killed_count > 0) {
        sem_opusc(g_sem_id, SEM_MAIN);
        g_shm->total_tourists_finished += killed_count;
        sem_podnies(g_sem_id, SEM_MAIN);
        logger(LOG_SYSTEM, "Wymuszono zakończenie %d turystów", killed_count);
    }

    logger(LOG_SYSTEM, "Generowanie raportu końcowego...");
    generuj_raport_koncowy();
    
    logger(LOG_SYSTEM, "Sprzątanie zasobów...");
    
    odlacz_pamiec(g_shm);
    usun_pamiec(g_shm_id);
    usun_semafory(g_sem_id);
    usun_kolejke(g_msg_id);
    usun_kolejke(g_msg_worker_id);
    
    logger_close();
    
    printf("\n============================================================\n");
    printf("        SYMULACJA ZAKOŃCZONA\n");
    printf("============================================================\n");
    printf("Logi zapisane do: kolej_log.txt\n");
    printf("Raport zapisany do: raport_karnetow.txt\n");
    printf("============================================================\n\n");


    return 0;
}
