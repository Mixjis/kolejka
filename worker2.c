#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/msg.h>
#include <pthread.h>
#include "struktury.h"
#include "operacje.h"
#include "logger.h"

// Flagi sygnałów
static volatile sig_atomic_t shutdown_flag = 0;
static volatile sig_atomic_t emergency_stop = 0;
static volatile sig_atomic_t emergency_resume = 0;

static int g_sem_id = -1;
static int g_msg_id = -1;
static int g_msg_worker_id = -1;
static SharedMemory* g_shm = NULL;

// Handler sygnałów
void worker2_signal_handler(int sig) {
    if (sig == SIGTERM) {
        shutdown_flag = 1;
    } else if (sig == SIGUSR1) {
        // Zatrzymanie awaryjne (od worker1 lub własne)
        emergency_stop = 1;
        emergency_resume = 0;
        // Log będzie w głównej pętli
    } else if (sig == SIGUSR2) {
        // Wznowienie po awarii
        emergency_resume = 1;
    }
}

// Wysyłanie sygnału do worker1
void send_signal_to_worker1(int sig) {
    sem_opusc(g_sem_id, SEM_MAIN);
    pid_t worker1_pid = g_shm->worker1_pid;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    if (worker1_pid > 0) {
        kill(worker1_pid, sig);
    }
}

// Inicjowanie zatrzymania awaryjnego przez worker2
void initiate_emergency_stop_w2(void) {
    logger(LOG_EMERGENCY, "PRACOWNIK2: Inicjuję AWARYJNE ZATRZYMANIE kolei!");
    
    // Ustaw lokalną flagę
    emergency_stop = 1;
    emergency_resume = 0;
    
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->emergency_stop = true;
    g_shm->emergency_initiator = 2;
    g_shm->worker1_ready = false;
    g_shm->worker2_ready = false;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Powiadom worker1
    send_signal_to_worker1(SIGUSR1);
    
    logger(LOG_EMERGENCY, "PRACOWNIK2: Kolej ZATRZYMANA - czekam na gotowość worker1");
}

// Wznowienie po awarii (gdy worker2 jest inicjatorem)
void resume_from_emergency_w2(void) {
    logger(LOG_EMERGENCY, "PRACOWNIK2: Sprawdzam gotowość do wznowienia...");
    
    // Oznacz gotowość
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->worker2_ready = true;
    bool worker1_ready = g_shm->worker1_ready;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Czekaj na worker1
    while (!worker1_ready && !shutdown_flag) {
        usleep(10000);
        sem_opusc(g_sem_id, SEM_MAIN);
        worker1_ready = g_shm->worker1_ready;
        sem_podnies(g_sem_id, SEM_MAIN);
    }
    
    if (shutdown_flag) return;
    
    // Poczekaj 5 sekund przed wznowieniem (używamy pętli usleep zamiast sleep)
    logger(LOG_EMERGENCY, "PRACOWNIK2: Worker1 gotowy - wznawiamy po 5s...");
    for (int i = 0; i < 50 && !shutdown_flag; i++) {
        usleep(100000); // 100ms x 50 = 5 sekund
    }
    
    // Wznów działanie
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->emergency_stop = false;
    g_shm->emergency_initiator = 0;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Powiadom worker1
    send_signal_to_worker1(SIGUSR2);
    
    emergency_stop = 0;
    emergency_resume = 0;
    
    logger(LOG_EMERGENCY, "PRACOWNIK2: Kolej WZNOWIONA - normalny ruch!");
}

// Licznik bramek wyjściowych (dla round-robin)
static int g_exit_gate_counter = 0;
static pthread_mutex_t g_exit_gate_mutex = PTHREAD_MUTEX_INITIALIZER;

// Pobierz następny numer bramki wyjściowej (1-2)
int get_next_exit_gate(void) {
    pthread_mutex_lock(&g_exit_gate_mutex);
    int gate = (g_exit_gate_counter % EXIT_GATES) + 1;
    g_exit_gate_counter++;
    pthread_mutex_unlock(&g_exit_gate_mutex);
    return gate;
}

// Wątek obsługi turysty na górnej stacji
typedef struct {
    int tourist_id;
    pid_t tourist_pid;
    TrailType trail;
} TouristExit;

void* tourist_exit_thread(void* arg) {
    TouristExit* te = (TouristExit*)arg;
    
    // Pobierz numer bramki
    int gate_num = get_next_exit_gate();
    
    // Czekaj na semafor wyjścia (2 bramki)
    sem_opusc(g_sem_id, SEM_GATE_EXIT);
    logger(LOG_WORKER2, "Turysta #%d wypuszczony przez bramkę wyjściową #%d", 
           te->tourist_id, gate_num);
    
    // Symulacja zjazdu trasą
    int trail_time;
    const char* trail_name;
    switch (te->trail) {
        case TRAIL_T1:
            trail_time = TRAIL_T1_TIME;
            trail_name = "T1 (łatwa)";
            break;
        case TRAIL_T2:
            trail_time = TRAIL_T2_TIME;
            trail_name = "T2 (średnia)";
            break;
        case TRAIL_T3:
        default:
            trail_time = TRAIL_T3_TIME;
            trail_name = "T3 (trudna)";
            break;
    }
    
    logger(LOG_WORKER2, "Turysta #%d zjeżdża trasą %s (%ds)", 
           te->tourist_id, trail_name, trail_time);
    
    // Aktualizuj statystyki tras
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->trail_usage[te->trail]++;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Symulacja zjazdu
    sleep(trail_time);
    
    // Wyślij powiadomienie do turysty, że zjazd zakończony i może wrócić
    Message msg;
    msg.mtype = te->tourist_pid;
    msg.sender_pid = getpid();
    msg.data = 3; // Zjazd zakończony (różne od 2 = dotarcie na górę)
    msg.tourist_id = te->tourist_id;
    wyslij_komunikat_nowait(g_msg_id, &msg);
    
    // Zwolnij bramkę wyjścia
    sem_podnies(g_sem_id, SEM_GATE_EXIT);
    
    logger(LOG_WORKER2, "Turysta #%d zakończył zjazd trasą %s i zjeżdża na dół", 
           te->tourist_id, trail_name);
    
    free(te);
    return NULL;
}

int main(void) {
    // Konfiguracja sygnałów
    struct sigaction sa;
    sa.sa_handler = worker2_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    
    // Połącz z zasobami IPC
    g_msg_id = polacz_kolejke();
    g_msg_worker_id = polacz_kolejke_worker();
    g_sem_id = polacz_semafory();
    int shm_id = polacz_pamiec();
    g_shm = dolacz_pamiec(shm_id);
    
    // Zapisz PID
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->worker2_pid = getpid();
    sem_podnies(g_sem_id, SEM_MAIN);
    
    srand(time(NULL) ^ getpid());
    
    logger(LOG_WORKER2, "Rozpoczynam pracę na stacji górnej!");
    
    Message msg;
    int emergency_timer = 0;
    bool should_trigger_emergency = false;
    int next_emergency = 3000 + (rand() % 2000); // Losowy czas do następnej awarii (3-5s)
    
    while (!shutdown_flag) {
        // Obsługa awarii - sprawdź czy trzeba zainicjować
        emergency_timer++;
        if (!emergency_stop && emergency_timer >= next_emergency) {
            // Losowa szansa na awarię (50%)
            if (rand() % 100 < 50) {
                should_trigger_emergency = true;
            }
            emergency_timer = 0;
            next_emergency = 3000 + (rand() % 2000); // Reset na 3-5s
        }
        
        if (should_trigger_emergency && !emergency_stop) {
            should_trigger_emergency = false;
            initiate_emergency_stop_w2();
        }
        
        // Obsługa zatrzymania awaryjnego
        if (emergency_stop) {
            // Sprawdź czy to my zainicjowaliśmy
            sem_opusc(g_sem_id, SEM_MAIN);
            int initiator = g_shm->emergency_initiator;
            sem_podnies(g_sem_id, SEM_MAIN);
            
            if (initiator == 2 && emergency_resume) {
                // My zainicjowaliśmy i dostaliśmy sygnał wznowienia
                resume_from_emergency_w2();
            } else if (initiator == 1) {
                // Worker1 zainicjował - loguj i odpowiedz gotowością
                logger(LOG_EMERGENCY, "PRACOWNIK2: Odebrano sygnał AWARII od worker1!");
                
                sem_opusc(g_sem_id, SEM_MAIN);
                g_shm->worker2_ready = true;
                sem_podnies(g_sem_id, SEM_MAIN);
                
                logger(LOG_WORKER2, "Potwierdzam gotowość do wznowienia (awaria od worker1)");
                
                // Czekaj na sygnał wznowienia
                while (emergency_stop && !emergency_resume && !shutdown_flag) {
                    usleep(10000);
                }
                
                if (emergency_resume) {
                    emergency_stop = 0;
                    emergency_resume = 0;
                    logger(LOG_EMERGENCY, "PRACOWNIK2: Otrzymano sygnał WZNOWIENIA od worker1!");
                }
            }
            
            if (emergency_stop) {
                usleep(10000);
                continue;
            }
        }
        
        // Sprawdź koniec dnia
        sem_opusc(g_sem_id, SEM_MAIN);
        bool is_running = g_shm->is_running;
        sem_podnies(g_sem_id, SEM_MAIN);
        
        if (!is_running) {
            logger(LOG_WORKER2, "Symulacja zakończona");
            break;
        }
        
        // Odbieraj krzesełka przyjeżdżające na górną stację (tylko logowanie)
        while (odbierz_komunikat(g_msg_worker_id, &msg, MSG_CHAIR_ARRIVAL, false)) {
            if (shutdown_flag || emergency_stop) break;
            
            int chair_id = msg.data;
            int passenger_count = msg.data2;
            
            logger(LOG_CHAIR, "Krzesełko #%d dotarło na górną stację z %d pasażerami",
                   chair_id, passenger_count);
            
            // Wyślij powiadomienie do pasażerów że dotarli (data == 2)
            for (int i = 0; i < passenger_count && i < CHAIR_CAPACITY; i++) {
                pid_t tourist_pid = msg.child_ids[i];
                if (tourist_pid <= 0) continue;
                
                Message reply;
                reply.mtype = tourist_pid;
                reply.sender_pid = getpid();
                reply.data = 2; // Dotarłeś na górę
                reply.tourist_id = chair_id * 100 + i;
                wyslij_komunikat_nowait(g_msg_id, &reply);
            }
        }
        
        // Odbieraj prośby turystów o wyjście
        while (odbierz_komunikat(g_msg_id, &msg, MSG_TOURIST_EXIT, false)) {
            if (shutdown_flag || emergency_stop) break;
            
            TouristExit* te = malloc(sizeof(TouristExit));
            te->tourist_pid = msg.sender_pid;
            te->tourist_id = msg.tourist_id;
            te->trail = (TrailType)msg.data; // Wybrana trasa
            
            pthread_t thread;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            
            if (pthread_create(&thread, &attr, tourist_exit_thread, te) != 0) {
                perror("Błąd tworzenia wątku wyjścia turysty");
                free(te);
            }
            
            pthread_attr_destroy(&attr);
        }
        
        usleep(1000); // 1ms
    }
    
    logger(LOG_WORKER2, "Kończę pracę na stacji górnej");
    
    odlacz_pamiec(g_shm);
    return 0;
}
