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
    int children_counts[CHAIR_CAPACITY];
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

#define MAX_WAITERS 15000
static PlatformWaiter waiters[MAX_WAITERS];
static int waiter_count = 0;
static pthread_mutex_t waiter_mutex = PTHREAD_MUTEX_INITIALIZER;

// Licznik bramek na peron
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

//  Dodaj oczekującego 
//  true - dodano 
//  false - kolejka pełna
bool add_waiter(PlatformWaiter* w) {
    pthread_mutex_lock(&waiter_mutex);
    if (waiter_count < MAX_WAITERS) {
        waiters[waiter_count++] = *w;
        pthread_mutex_unlock(&waiter_mutex);
        return true;
    }
    pthread_mutex_unlock(&waiter_mutex);
    logger(LOG_WORKER1, "[ERROR] Kolejka waiters pełna! Turysta #%d NIE dodany!", w->tourist_id);
    return false;
}

// Sprawdzenie czy kombinacja jest dozwolona
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
    
    // Próba zabrania grupy (Max 4 osoby)
    int indices_to_remove[CHAIR_CAPACITY];
    int remove_count = 0;
    
    for (int i = 0; i < waiter_count && group->count < CHAIR_CAPACITY; i++) {
        PlatformWaiter* w = &waiters[i];
        
        int new_cyclists = group->cyclists + (w->type == TOURIST_CYCLIST ? 1 : 0);
        int new_pedestrians = group->pedestrians + (w->type == TOURIST_PEDESTRIAN ? 1 : 0);
        
        // Dodaj dzieci jako pieszych
        if (w->type == TOURIST_CYCLIST) {
            new_cyclists += w->children_count;
        } else {
            new_pedestrians += w->children_count;
        }

        
        if (is_valid_combination(new_cyclists, new_pedestrians)) {
            // Dodaj do grupy
            group->tourist_ids[group->count] = w->tourist_id;
            group->tourist_pids[group->count] = w->pid;
            group->tourist_types[group->count] = w->type;
            group->children_counts[group->count] = w->children_count;
            group->count++;
            
            if (w->type == TOURIST_CYCLIST) {
                group->cyclists++;
            } else {
                group->pedestrians++;
            }
            
            // Dodaj dzieci
            if (w->type == TOURIST_CYCLIST) {
                // Dzieci rowerzysty też są rowerzystami
                group->cyclists += w->children_count;
            } else {
                // Dzieci pieszego też są pieszymi
                group->pedestrians += w->children_count;
            }
            
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
    
    int travel_time = CHAIR_TRAVEL_TIME;
    int time_traveled = 0;

    // Obliczanie liczby pasażerów z dziećmi
    int total_children = 0;
    for (int i = 0; i < group->count; i++) {
        total_children += group->children_counts[i];
    }
    int total_passengers = group->count + total_children;

    // Aktualizuj statystyki
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->chair_departures++;
    g_shm->passengers_transported += total_passengers;
    g_shm->cyclists_transported += group->cyclists;
    g_shm->pedestrians_transported += group->pedestrians;
    g_shm->active_chairs++;
    int chair_id = g_shm->chair_departures;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Log odjazdu
    char passengers_str[256] = "";
    for (int i = 0; i < group->count; i++) {
        char tmp[64];
        if (group->children_counts[i] > 0) {
            snprintf(tmp, sizeof(tmp), "%s#%d%s+%ddz", 
                     i > 0 ? ", " : "",
                     group->tourist_ids[i],
                     group->tourist_types[i] == TOURIST_CYCLIST ? "(R)" : "(P)",
                     group->children_counts[i]);
        } else {
            snprintf(tmp, sizeof(tmp), "%s#%d%s", 
                     i > 0 ? ", " : "",
                     group->tourist_ids[i],
                     group->tourist_types[i] == TOURIST_CYCLIST ? "(R)" : "(P)");
        }
        strcat(passengers_str, tmp);
    }
    
    logger(LOG_CHAIR, "Krzesełko #%d odjeżdża z pasażerami: [%s] (R:%d, P:%d)",
           chair_id, passengers_str, group->cyclists, group->pedestrians);
    
    // Symulacja przejazdu
    while (time_traveled < travel_time && !shutdown_flag) {
        // sprawdzanie czy jest awaria
        sem_opusc(g_sem_id, SEM_MAIN);
        bool emergency = g_shm->emergency_stop;
        sem_podnies(g_sem_id, SEM_MAIN);
        
        if (emergency) {
            // zatrzymanie krzesełka
            logger(LOG_CHAIR, "Krzesełko #%d ZATRZYMANE w trakcie jazdy! (przejechane: %d/%d s)",
                   chair_id, time_traveled, travel_time);
            
            // Czekanie na koniec awarii
            while (emergency && !shutdown_flag) {
                sem_opusc(g_sem_id, SEM_MAIN);
                emergency = g_shm->emergency_stop;
                sem_podnies(g_sem_id, SEM_MAIN);
            }
            
            if (!shutdown_flag) {
                logger(LOG_CHAIR, "Krzesełko #%d WZNAWIA jazdę (pozostało: %d s)",
                       chair_id, travel_time - time_traveled);
            }
        } else {
            // Normalny ruch
            time_t second_start = time(NULL);
            bool interrupted = false;
            
            // Czekanie 1 sekundy, ze sprawdzaniem awarii
            while ((time(NULL) - second_start) < 1 && !shutdown_flag) {
                // Sprawdzanie czy nie wystąpiła awaria w trakcie tej sekundy
                sem_opusc(g_sem_id, SEM_MAIN);
                emergency = g_shm->emergency_stop;
                sem_podnies(g_sem_id, SEM_MAIN);
                
                if (emergency) {
                    interrupted = true;
                    break;
                }
            }
            
            // Jeśli sekunda minęła bez przerwania - doliczam
            if (!interrupted && !shutdown_flag) {
                time_traveled++;
            }
            // Jeśli przerwana awarią - powrót do pętli głównej
        }
    }

    // Przyjazd na górę - wysłanie komunikatu do worker2
    Message msg;
    msg.mtype = MSG_CHAIR_ARRIVAL;
    msg.sender_pid = getpid();
    msg.data = chair_id;
    msg.data2 = group->count;
    
    // kopiowanie danych pasażerów do wiadomości
    for (int i = 0; i < CHAIR_CAPACITY; i++) {
        if (i < group->count) {
            msg.child_ids[i] = group->tourist_pids[i];
        } else {
            msg.child_ids[i] = 0;
        }
    }
    
    wyslij_komunikat(g_msg_worker_id, &msg);
    
    // Aktualizacja statystyk
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->active_chairs--;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Zwolnienie semafora krzesełka
    sem_podnies_bez_undo(g_sem_id, SEM_CHAIRS);
    
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
    
    // Powiadomienie worker2
    send_emergency_to_worker2(true);
    
    // Zablokuj semafor awaryjny (blokuje wypuszczanie krzesełek)
    sem_ustaw_wartosc(g_sem_id, SEM_EMERGENCY, 0);
}

// Wznowienie po awarii
void resume_from_emergency(void) {

    // Oznacz gotowość
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->worker1_ready = true;
    bool worker2_ready = g_shm->worker2_ready;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Czekaj na worker2
    while (!worker2_ready && !shutdown_flag) {
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

// Odbierz i dodaj turystów do kolejki waiters
// Zwraca liczbę odebranych komunikatów
int receive_platform_messages(Message* msg) {
    int received = 0;
    
    while (odbierz_komunikat(g_msg_id, msg, MSG_TOURIST_TO_PLATFORM, false)) {
        if (shutdown_flag) break;
        
        PlatformWaiter w;
        w.pid = msg->sender_pid;
        w.tourist_id = msg->tourist_id;
        w.type = msg->tourist_type;
        w.children_count = msg->children_count;
        w.child_ids[0] = msg->child_ids[0];
        w.child_ids[1] = msg->child_ids[1];
    
        int gate_num = get_next_platform_gate();

        if (add_waiter(&w)) {
            sem_opusc(g_sem_id, SEM_MAIN);
            g_shm->tourists_on_platform++;
            sem_podnies(g_sem_id, SEM_MAIN);

            logger(LOG_WORKER1, "Turysta #%d wpuszczony przez bramkę peronową #%d (typ: %s, dzieci: %d)",
                   w.tourist_id, gate_num,
                   w.type == TOURIST_CYCLIST ? "rowerzysta" : "pieszy",
                   w.children_count);
        } else {
            Message refuse;
            refuse.mtype = w.pid;
            refuse.sender_pid = getpid();
            refuse.data = -1;
            refuse.tourist_id = w.tourist_id;
            wyslij_komunikat(g_msg_id, &refuse);
        }
        
        received++;
    }
    
    return received;
}

// Wyślij jedno krzesełko jeśli możliwe
// Zwraca true jeśli wysłano krzesełko
bool dispatch_one_chair(void) {
    if (emergency_stop) return false;
    
    pthread_mutex_lock(&waiter_mutex);
    int current_waiters = waiter_count;
    pthread_mutex_unlock(&waiter_mutex);
    
    if (current_waiters == 0) return false;
    
    // Sprawdź dostępność krzesełka
    int result = sem_probuj_opusc_bez_undo(g_sem_id, SEM_CHAIRS);
    if (result != 1) return false;
    
    ChairGroup* group = malloc(sizeof(ChairGroup));
    if (!try_create_group(group)) {
        free(group);
        sem_podnies_bez_undo(g_sem_id, SEM_CHAIRS);
        return false;
    }
    
    // Powiadom turystów o wsiadaniu 
    for (int i = 0; i < group->count; i++) {
        Message notify;
        notify.mtype = group->tourist_pids[i];
        notify.sender_pid = getpid();
        notify.data = 1; // OK wsiadaj
        notify.tourist_id = group->tourist_ids[i];
        wyslij_komunikat(g_msg_id, &notify);
        
        // Log wpuszczenia turysty
        if (group->children_counts[i] > 0) {
            logger(LOG_WORKER1, "Wpuszczam turyste #%d%s+%ddz na krzesełko",
                   group->tourist_ids[i],
                   group->tourist_types[i] == TOURIST_CYCLIST ? "(R)" : "(P)",
                   group->children_counts[i]);
        } else {
            logger(LOG_WORKER1, "Wpuszczam turyste #%d%s na krzesełko",
                   group->tourist_ids[i],
                   group->tourist_types[i] == TOURIST_CYCLIST ? "(R)" : "(P)");
        }
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
        sem_podnies_bez_undo(g_sem_id, SEM_CHAIRS);
        pthread_attr_destroy(&attr);
        return false;
    }

    pthread_attr_destroy(&attr);
    return true;
}


int main(void) {
    // Inicjalizacja loggera dla procesu potomnego
    logger_init_child();
    
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
    time_t sim_start = g_shm->simulation_start;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    srand(time(NULL) ^ getpid());
    
    // Opóźnienie rozpoczęcia pracy pracownika o WORK_START_TIME sekund
    logger(LOG_WORKER1, "Czekam %d sekund przed rozpoczęciem pracy...", WORK_START_TIME);
    
    while (!shutdown_flag) {
        time_t now = time(NULL);
        time_t elapsed = now - sim_start;
        
        if (elapsed >= WORK_START_TIME) {
            break;
        }
    }
    
    if (shutdown_flag) {
        logger(LOG_WORKER1, "Przerwano przed rozpoczęciem pracy");
        odlacz_pamiec(g_shm);
        return 0;
    }
    
    logger(LOG_WORKER1, "Rozpoczynam pracę na stacji dolnej!");
    
    Message msg;
    bool should_trigger_emergency = false;
    
    // System awarii
    time_t last_emergency_check = time(NULL);
    int next_emergency_delay = 3 + (rand() % 3);  // 3-5 sekund
    
    while (!shutdown_flag) {
        // Sprawdź czy bramki zamknięte (koniec dnia)
        sem_opusc(g_sem_id, SEM_MAIN);
        bool gates_closed = g_shm->gates_closed;
        int on_platform = g_shm->tourists_on_platform;
        int active_chairs = g_shm->active_chairs;
        sem_podnies(g_sem_id, SEM_MAIN);
        
        // === OBSŁUGA AWARII ===
        if (!gates_closed) {
            time_t now = time(NULL);
            time_t elapsed_since_start = now - sim_start;
            time_t time_to_end = WORK_END_TIME - elapsed_since_start;
            
            // Sprawdź czy minął czas od ostatniej próby awarii
            if (!emergency_stop && (now - last_emergency_check) >= next_emergency_delay) {
                // Nie inicjuj awarii jeśli zostało mniej niż EMERGENCY_SAFETY_MARGIN sekund do końca
                if (time_to_end > EMERGENCY_SAFETY_MARGIN) {
                    if (rand() % 100 < EMERGENCY_CHANCE) {
                        should_trigger_emergency = true;
                    }
                }
                last_emergency_check = now;
                next_emergency_delay = 3 + (rand() % 3);
            }
            
            if (should_trigger_emergency && !emergency_stop) {
                should_trigger_emergency = false;
                initiate_emergency_stop();
            }
        }
        
        // obsługa awarii
        if (emergency_stop) {
            // Podczas awarii nadal odbieraj turystów do kolejki
            receive_platform_messages(&msg);
            
            sem_opusc(g_sem_id, SEM_MAIN);
            int initiator = g_shm->emergency_initiator;
            bool w2_ready = g_shm->worker2_ready;
            sem_podnies(g_sem_id, SEM_MAIN);
            
            if (initiator == 1) {
                // My zainicjowaliśmy - czekaj na worker2
                while (!w2_ready && !shutdown_flag) {
                    receive_platform_messages(&msg);
                    
                    sem_opusc(g_sem_id, SEM_MAIN);
                    w2_ready = g_shm->worker2_ready;
                    sem_podnies(g_sem_id, SEM_MAIN);
                }
                
                if (w2_ready && !shutdown_flag) {
                    logger(LOG_EMERGENCY, "PRACOWNIK1: Worker2 gotowy - Zatrzymanie ruchu kolei...");
                    
                    time_t start_time = time(NULL);
                    while(time(NULL) - start_time < EMERGENCY_DURATION && !shutdown_flag) {
                        //receive_platform_messages(&msg);
                    }
                    
                    resume_from_emergency();
                }
            } else if (initiator == 2) {
                // Worker2 zainicjował
                logger(LOG_EMERGENCY, "PRACOWNIK1: Odebrano sygnał AWARII od worker2!");

                sem_opusc(g_sem_id, SEM_MAIN);
                g_shm->worker1_ready = true;
                sem_podnies(g_sem_id, SEM_MAIN);
                
                logger(LOG_EMERGENCY, "PRACOWNIK1: Potwierdzam gotowość (awaria od worker2)");
                
                while (emergency_stop && !emergency_resume && !shutdown_flag) {
                    receive_platform_messages(&msg);
                }
                
                if (emergency_resume) {
                    emergency_stop = 0;
                    emergency_resume = 0;
                    logger(LOG_EMERGENCY, "Otrzymano sygnał wznowienia od worker2");
                }
            }
            continue;
        }
        
        // Sprawdź czy zakończyć pracę
        if (shutdown_flag) {
            // Wymuszony shutdown
            Message cleanup_msg;
            while (odbierz_komunikat(g_msg_id, &cleanup_msg, MSG_TOURIST_TO_PLATFORM, false)) {
                Message refuse;
                refuse.mtype = cleanup_msg.sender_pid;
                refuse.sender_pid = getpid();
                refuse.data = -1;
                refuse.tourist_id = cleanup_msg.tourist_id;
                wyslij_komunikat(g_msg_id, &refuse);
            }

            pthread_mutex_lock(&waiter_mutex);
            for (int i = 0; i < waiter_count; i++) {
                Message refuse;
                refuse.mtype = waiters[i].pid;
                refuse.sender_pid = getpid();
                refuse.data = -1;
                refuse.tourist_id = waiters[i].tourist_id;
                wyslij_komunikat(g_msg_id, &refuse);
            }
            waiter_count = 0;
            pthread_mutex_unlock(&waiter_mutex);

            logger(LOG_WORKER1, "Wymuszony shutdown - zamykam stację dolną");
            break;
        }

        // Normalne zakończenie - bramki zamknięte, peron pusty, kolejka pusta, krzesełka wróciły
        pthread_mutex_lock(&waiter_mutex);
        int current_waiters = waiter_count;
        pthread_mutex_unlock(&waiter_mutex);
        
        if (gates_closed && on_platform == 0 && current_waiters == 0 && active_chairs == 0) {
            logger(LOG_WORKER1, "Koniec dnia - wszyscy turyści obsłużeni");
            break;
        }
        
        // naprzemienne wpuszczanie na peron i wysyłanie krzesełek
        
        // odbieranie komunikatów od turystów
        int received = 0;
        for (int i = 0; i < 5; i++) {
            if (odbierz_komunikat(g_msg_id, &msg, MSG_TOURIST_TO_PLATFORM, false)) {
                PlatformWaiter w;
                w.pid = msg.sender_pid;
                w.tourist_id = msg.tourist_id;
                w.type = msg.tourist_type;
                w.children_count = msg.children_count;
                w.child_ids[0] = msg.child_ids[0];
                w.child_ids[1] = msg.child_ids[1];
            
                int gate_num = get_next_platform_gate();

                if (add_waiter(&w)) {
                    sem_opusc(g_sem_id, SEM_MAIN);
                    g_shm->tourists_on_platform++;
                    sem_podnies(g_sem_id, SEM_MAIN);

                    logger(LOG_WORKER1, "Turysta #%d wpuszczony przez bramkę peronową #%d (typ: %s, dzieci: %d)",
                           w.tourist_id, gate_num,
                           w.type == TOURIST_CYCLIST ? "rowerzysta" : "pieszy",
                           w.children_count);
                } else {
                    Message refuse;
                    refuse.mtype = w.pid;
                    refuse.sender_pid = getpid();
                    refuse.data = -1;
                    refuse.tourist_id = w.tourist_id;
                    wyslij_komunikat(g_msg_id, &refuse);
                }
                received++;
            } else {
                break;  // Nie ma więcej komunikatów
            }
        }

        // wysyłanie krzesełek
        for (int i = 0; i < 3; i++) {
            if (!dispatch_one_chair()) {
                break;  // Nie udało się wysłać
            }
        }
        
        // Próba wysłania jeśli dalej są czekający
        pthread_mutex_lock(&waiter_mutex);
        current_waiters = waiter_count;
        pthread_mutex_unlock(&waiter_mutex);
        
        if (received == 0 && current_waiters > 0) {
            dispatch_one_chair();
        }
    }
    
    logger(LOG_WORKER1, "Kończę pracę na stacji dolnej");
    
    odlacz_pamiec(g_shm);
    return 0;
}