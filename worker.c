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
void worker1_signal_handler(int sig) {
    if (sig == SIGTERM) {
        shutdown_flag = 1;
    } else if (sig == SIGUSR1) {
        // Zatrzymanie awaryjne (od worker2 lub main)
        emergency_stop = 1;
        emergency_resume = 0;
    } else if (sig == SIGUSR2) {
        // Wznowienie po awarii
        emergency_resume = 1;
    }
}

// Struktura grupy na krzesełko
typedef struct {
    int tourist_ids[CHAIR_CAPACITY];
    pid_t tourist_pids[CHAIR_CAPACITY];
    TouristType tourist_types[CHAIR_CAPACITY];
    int count;
    int cyclists;
    int pedestrians;
} ChairGroup;

// Kolejka oczekujących na peron
typedef struct {
    pid_t pid;
    int tourist_id;
    TouristType type;
    int children_count;
    int child_ids[2];
} PlatformWaiter;

#define MAX_WAITERS 500
static PlatformWaiter waiters[MAX_WAITERS];
static int waiter_count = 0;
static pthread_mutex_t waiter_mutex = PTHREAD_MUTEX_INITIALIZER;

// Licznik bramek na peron (dla round-robin)
static int g_platform_gate_counter = 0;
static pthread_mutex_t g_platform_gate_mutex = PTHREAD_MUTEX_INITIALIZER;

// Pobierz następny numer bramki na peron (1-3)
int get_next_platform_gate(void) {
    pthread_mutex_lock(&g_platform_gate_mutex);
    int gate = (g_platform_gate_counter % PLATFORM_GATES) + 1;
    g_platform_gate_counter++;
    pthread_mutex_unlock(&g_platform_gate_mutex);
    return gate;
}

// Dodaj oczekującego
void add_waiter(PlatformWaiter* w) {
    pthread_mutex_lock(&waiter_mutex);
    if (waiter_count < MAX_WAITERS) {
        waiters[waiter_count++] = *w;
    }
    pthread_mutex_unlock(&waiter_mutex);
}

// Sprawdź czy kombinacja jest dozwolona
// Max 2 rowerzystów LUB 1 rowerzysta + 2 pieszych LUB 4 pieszych
bool is_valid_combination(int cyclists, int pedestrians) {
    int total = cyclists + pedestrians;
    if (total > CHAIR_CAPACITY) return false;
    if (cyclists > 2) return false;
    if (cyclists == 2 && pedestrians > 0) return false;
    if (cyclists == 1 && pedestrians > 2) return false;
    return true;
}

// Spróbuj utworzyć grupę na krzesełko
bool try_create_group(ChairGroup* group) {
    pthread_mutex_lock(&waiter_mutex);
    
    if (waiter_count == 0) {
        pthread_mutex_unlock(&waiter_mutex);
        return false;
    }
    
    memset(group, 0, sizeof(ChairGroup));
    
    // Próbuj zebrać grupę (max 4 osoby)
    int indices_to_remove[CHAIR_CAPACITY];
    int remove_count = 0;
    
    for (int i = 0; i < waiter_count && group->count < CHAIR_CAPACITY; i++) {
        PlatformWaiter* w = &waiters[i];
        
        int new_cyclists = group->cyclists + (w->type == TOURIST_CYCLIST ? 1 : 0);
        int new_pedestrians = group->pedestrians + (w->type == TOURIST_PEDESTRIAN ? 1 : 0);
        
        // Dodaj dzieci jako pieszych
        new_pedestrians += w->children_count;
        
        if (is_valid_combination(new_cyclists, new_pedestrians)) {
            // Dodaj do grupy
            group->tourist_ids[group->count] = w->tourist_id;
            group->tourist_pids[group->count] = w->pid;
            group->tourist_types[group->count] = w->type;
            group->count++;
            
            if (w->type == TOURIST_CYCLIST) {
                group->cyclists++;
            } else {
                group->pedestrians++;
            }
            
            // Dodaj dzieci
            group->pedestrians += w->children_count;
            
            indices_to_remove[remove_count++] = i;
        }
    }
    
    // Usuń dodanych z kolejki (od końca)
    for (int i = remove_count - 1; i >= 0; i--) {
        int idx = indices_to_remove[i];
        for (int j = idx; j < waiter_count - 1; j++) {
            waiters[j] = waiters[j + 1];
        }
        waiter_count--;
    }
    
    pthread_mutex_unlock(&waiter_mutex);
    
    return (group->count > 0);
}

// Wątek obsługi krzesełka
void* chair_thread(void* arg) {
    ChairGroup* group = (ChairGroup*)arg;
    
    // Symulacja przejazdu
    int travel_time = CHAIR_TRAVEL_TIME;
    
    // Aktualizuj statystyki
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->chair_departures++;
    g_shm->passengers_transported += group->count;
    g_shm->cyclists_transported += group->cyclists;
    g_shm->pedestrians_transported += group->pedestrians;
    g_shm->active_chairs++;
    int chair_id = g_shm->chair_departures;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Log odjazdu
    char passengers_str[256] = "";
    for (int i = 0; i < group->count; i++) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%s#%d%s", 
                 i > 0 ? ", " : "",
                 group->tourist_ids[i],
                 group->tourist_types[i] == TOURIST_CYCLIST ? "(R)" : "(P)");
        strcat(passengers_str, tmp);
    }
    
    logger(LOG_CHAIR, "Krzesełko #%d odjeżdża z %d pasażerami: [%s] (R:%d, P:%d)",
           chair_id, group->count, passengers_str, group->cyclists, group->pedestrians);
    
    // Symulacja przejazdu
    sleep(travel_time);
    
    // Przyjazd na górę - wyślij komunikat do worker2
    Message msg;
    msg.mtype = MSG_CHAIR_ARRIVAL;
    msg.sender_pid = getpid();
    msg.data = chair_id;
    msg.data2 = group->count;
    
    // Skopiuj dane pasażerów do wiadomości (wszystkie 4 PID-y)
    for (int i = 0; i < CHAIR_CAPACITY; i++) {
        if (i < group->count) {
            msg.child_ids[i] = group->tourist_pids[i];
        } else {
            msg.child_ids[i] = 0;
        }
    }
    
    wyslij_komunikat(g_msg_worker_id, &msg);
    
    // Aktualizuj statystyki
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->active_chairs--;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Zwolnij semafor krzesełka
    sem_podnies(g_sem_id, SEM_CHAIRS);
    
    free(group);
    return NULL;
}

// Wysyłanie sygnału awaryjnego do worker2
void send_emergency_to_worker2(bool stop) {
    sem_opusc(g_sem_id, SEM_MAIN);
    pid_t worker2_pid = g_shm->worker2_pid;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    if (worker2_pid > 0) {
        kill(worker2_pid, stop ? SIGUSR1 : SIGUSR2);
    }
}

// Inicjowanie zatrzymania awaryjnego
void initiate_emergency_stop(void) {
    logger(LOG_EMERGENCY, "PRACOWNIK1: Inicjuję AWARYJNE ZATRZYMANIE kolei!");
    
    // Ustaw lokalną flagę
    emergency_stop = 1;
    emergency_resume = 0;
    
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->emergency_stop = true;
    g_shm->emergency_initiator = 1;
    g_shm->worker1_ready = false;
    g_shm->worker2_ready = false;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Powiadom worker2
    send_emergency_to_worker2(true);
    
    // Zablokuj semafor awaryjny (blokuje wypuszczanie krzesełek)
    sem_ustaw_wartosc(g_sem_id, SEM_EMERGENCY, 0);
    
    logger(LOG_EMERGENCY, "PRACOWNIK1: Kolej ZATRZYMANA - czekam na gotowość worker2");
}

// Wznowienie po awarii
void resume_from_emergency(void) {
    logger(LOG_EMERGENCY, "PRACOWNIK1: Sprawdzam gotowość do wznowienia...");
    
    // Oznacz gotowość
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->worker1_ready = true;
    bool worker2_ready = g_shm->worker2_ready;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Czekaj na worker2
    while (!worker2_ready && !shutdown_flag) {
        usleep(10000);
        sem_opusc(g_sem_id, SEM_MAIN);
        worker2_ready = g_shm->worker2_ready;
        sem_podnies(g_sem_id, SEM_MAIN);
    }
    
    if (shutdown_flag) return;
    
    // Wznów działanie
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->emergency_stop = false;
    g_shm->emergency_initiator = 0;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Odblokuj semafor awaryjny
    sem_ustaw_wartosc(g_sem_id, SEM_EMERGENCY, 1);
    
    // Powiadom worker2
    send_emergency_to_worker2(false);
    
    emergency_stop = 0;
    emergency_resume = 0;
    
    logger(LOG_EMERGENCY, "PRACOWNIK1: Kolej WZNOWIONA - normalny ruch!");
}

int main(void) {
    // Konfiguracja sygnałów
    struct sigaction sa;
    sa.sa_handler = worker1_signal_handler;
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
    g_shm->worker1_pid = getpid();
    sem_podnies(g_sem_id, SEM_MAIN);
    
    srand(time(NULL) ^ getpid());
    
    logger(LOG_WORKER1, "Rozpoczynam pracę na stacji dolnej!");
    
    Message msg;
    int emergency_timer = 0;
    bool should_trigger_emergency = false;
    int next_emergency = 3000 + (rand() % 2000); // Losowy czas do następnej awarii (3-5s)
    
    while (!shutdown_flag) {
        // Obsługa awarii - sprawdź czy trzeba zainicjować
        emergency_timer++;
        if (!emergency_stop && emergency_timer >= next_emergency) {
            // Losowa szansa na awarię (30%)
            if (rand() % 100 < 30) {
                should_trigger_emergency = true;
            }
            emergency_timer = 0;
            next_emergency = 3000 + (rand() % 2000); // Reset na 3-5s
        }
        
        if (should_trigger_emergency && !emergency_stop) {
            should_trigger_emergency = false;
            initiate_emergency_stop();
        }
        
        // Obsługa zatrzymania awaryjnego
        if (emergency_stop) {
            // Sprawdź kto zainicjował
            sem_opusc(g_sem_id, SEM_MAIN);
            int initiator = g_shm->emergency_initiator;
            bool w2_ready = g_shm->worker2_ready;
            sem_podnies(g_sem_id, SEM_MAIN);
            
            if (initiator == 1) {
                // My zainicjowaliśmy - czekaj na worker2 i wznów
                logger(LOG_EMERGENCY, "PRACOWNIK1: Awaria aktywna - czekam na worker2...");
                
                // Czekaj aż worker2 potwierdzi gotowość
                int wait_count = 0;
                while (!w2_ready && !shutdown_flag && wait_count < 300) {
                    usleep(10000); // 10ms
                    wait_count++;
                    sem_opusc(g_sem_id, SEM_MAIN);
                    w2_ready = g_shm->worker2_ready;
                    sem_podnies(g_sem_id, SEM_MAIN);
                }
                
                if (w2_ready || wait_count >= 300) {
                    // Worker2 gotowy lub timeout - wznów
                    logger(LOG_EMERGENCY, "PRACOWNIK1: Worker2 gotowy - wznawiamy po 5s...");
                    
                    // Używamy pętli z usleep zamiast sleep (sleep jest przerywany przez sygnały)
                    for (int i = 0; i < 50 && !shutdown_flag; i++) {
                        usleep(100000); // 100ms x 50 = 5 sekund
                    }
                    
                    resume_from_emergency();
                }
            } else if (initiator == 2) {
                // Worker2 zainicjował - odpowiedz gotowością i czekaj
                sem_opusc(g_sem_id, SEM_MAIN);
                g_shm->worker1_ready = true;
                sem_podnies(g_sem_id, SEM_MAIN);
                
                logger(LOG_WORKER1, "Potwierdzam gotowość do wznowienia (awaria od worker2)");
                
                // Czekaj na sygnał wznowienia
                while (emergency_stop && !emergency_resume && !shutdown_flag) {
                    usleep(10000);
                }
                
                if (emergency_resume) {
                    emergency_stop = 0;
                    emergency_resume = 0;
                    logger(LOG_WORKER1, "Otrzymano sygnał wznowienia od worker2");
                }
            }
            continue;
        }
        
        // Sprawdź koniec dnia
        sem_opusc(g_sem_id, SEM_MAIN);
        bool gates_closed = g_shm->gates_closed;
        int on_platform = g_shm->tourists_on_platform;
        int in_station = g_shm->tourists_in_station;
        sem_podnies(g_sem_id, SEM_MAIN);
        
        if (gates_closed && on_platform == 0 && in_station == 0 && waiter_count == 0) {
            logger(LOG_WORKER1, "Koniec dnia - wszyscy obsłużeni");
            break;
        }
        
        // Odbierz turystów chcących wejść na peron
        while (odbierz_komunikat(g_msg_id, &msg, MSG_TOURIST_TO_PLATFORM, false)) {
            if (shutdown_flag || emergency_stop) break;
            
            PlatformWaiter w;
            w.pid = msg.sender_pid;
            w.tourist_id = msg.tourist_id;
            w.type = msg.tourist_type;
            w.children_count = msg.children_count;
            w.child_ids[0] = msg.child_ids[0];
            w.child_ids[1] = msg.child_ids[1];
            
            // Pobierz numer bramki na peron
            int gate_num = get_next_platform_gate();
            
            add_waiter(&w);
            
            // Rejestruj przejście przez bramkę
            sem_opusc(g_sem_id, SEM_MAIN);
            g_shm->gate_passages_count++;
            g_shm->tourists_on_platform++;
            sem_podnies(g_sem_id, SEM_MAIN);
            
            logger(LOG_WORKER1, "Turysta #%d wpuszczony przez bramkę peronową #%d (typ: %s, dzieci: %d)",
                   w.tourist_id, gate_num, 
                   w.type == TOURIST_CYCLIST ? "rowerzysta" : "pieszy",
                   w.children_count);
        }
        
        // Próbuj utworzyć grupę i wysłać krzesełko
        if (!emergency_stop && waiter_count > 0) {
            // Sprawdź dostępność krzesełka
            int result = sem_probuj_opusc(g_sem_id, SEM_CHAIRS);
            if (result == 1) {
                ChairGroup* group = malloc(sizeof(ChairGroup));
                if (try_create_group(group)) {
                    // Powiadom turystów o wsiadaniu
                    for (int i = 0; i < group->count; i++) {
                        Message notify;
                        notify.mtype = group->tourist_pids[i];
                        notify.sender_pid = getpid();
                        notify.data = 1; // OK wsiadaj
                        notify.tourist_id = group->tourist_ids[i];
                        wyslij_komunikat_nowait(g_msg_id, &notify);
                    }
                    
                    // Aktualizuj licznik na peronie
                    sem_opusc(g_sem_id, SEM_MAIN);
                    g_shm->tourists_on_platform -= group->count;
                    sem_podnies(g_sem_id, SEM_MAIN);
                    
                    // Uruchom wątek krzesełka
                    pthread_t thread;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                    
                    if (pthread_create(&thread, &attr, chair_thread, group) != 0) {
                        perror("Błąd tworzenia wątku krzesełka");
                        free(group);
                        sem_podnies(g_sem_id, SEM_CHAIRS);
                    }
                    
                    pthread_attr_destroy(&attr);
                } else {
                    free(group);
                    sem_podnies(g_sem_id, SEM_CHAIRS);
                }
            }
        }
        
        usleep(1000); // 1ms
    }
    
    logger(LOG_WORKER1, "Kończę pracę na stacji dolnej");
    
    odlacz_pamiec(g_shm);
    return 0;
}
