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

// Globalne zmienne dla wątków
static volatile sig_atomic_t shutdown_flag = 0;
static volatile sig_atomic_t emergency_flag = 0;

static int g_sem_id = -1;
static int g_msg_id = -1;
static SharedMemory* g_shm = NULL;
static int g_tourist_id = -1;
static pid_t g_pid = 0;

// Dane turysty
static int g_age = 0;
static TouristType g_type = TOURIST_PEDESTRIAN;
static bool g_is_vip = false;
static int g_ticket_id = -1;
static TicketType g_ticket_type = TICKET_SINGLE;
static time_t g_ticket_valid_until = 0;
static int g_children_count = 0;
static int g_child_ages[2] = {0, 0};

// Struktura dziecka (realizowana wątkiem)
typedef struct {
    int child_index;
    int child_age;
    pthread_t thread;
    volatile bool on_chair;
    volatile bool finished;
} ChildThread;

static ChildThread g_children[2];
static pthread_mutex_t children_mutex = PTHREAD_MUTEX_INITIALIZER;

// Licznik bramek wejściowych lokalny
static int g_entry_gate = 0;

// Handler sygnałów
void tourist_signal_handler(int sig) {
    if (sig == SIGTERM) {
        shutdown_flag = 1;
    } else if (sig == SIGUSR1) {
        emergency_flag = 1;
    } else if (sig == SIGUSR2) {
        emergency_flag = 0;
    }
}

// Funkcja wątku dziecka - dziecko podąża za rodzicem
void* child_thread_func(void* arg) {
    ChildThread* child = (ChildThread*)arg;
    
    logger(LOG_TOURIST, "Dziecko #%d (wiek %d) turysty #%d - podąża z opiekunem",
           child->child_index, child->child_age, g_tourist_id);
    
    // Dziecko czeka aż rodzic zakończy
    while (!child->finished && !shutdown_flag) {
        usleep(10000);
    }
    
    return NULL;
}

// Uruchom wątki dzieci
void start_children_threads(void) {
    for (int i = 0; i < g_children_count; i++) {
        g_children[i].child_index = i;
        g_children[i].child_age = g_child_ages[i];
        g_children[i].on_chair = false;
        g_children[i].finished = false;
        
        if (pthread_create(&g_children[i].thread, NULL, child_thread_func, &g_children[i]) != 0) {
            perror("Błąd tworzenia wątku dziecka");
        }
    }
}

// Zakończ wątki dzieci
void finish_children_threads(void) {
    // Ustawienie flagi finished dla wszystkich dzieci
    pthread_mutex_lock(&children_mutex);
    for (int i = 0; i < g_children_count; i++) {
        g_children[i].finished = true;
    }
    pthread_mutex_unlock(&children_mutex);
    
    // Danie wątkom chwilę na zakończenie
    usleep(20000); // 20ms - 2 cykle pętli dziecka
    
    // Dołączenie do wątków (powinny już być zakończone)
    for (int i = 0; i < g_children_count; i++) {
        pthread_join(g_children[i].thread, NULL);
    }
}

// Kupno biletu
bool buy_ticket(void) {
    Message msg;
    
    // Sprawdź czy kasa jest otwarta przed próbą kupna biletu
    sem_opusc(g_sem_id, SEM_MAIN);
    bool cashier_open = g_shm->cashier_open;
    bool gates_closed = g_shm->gates_closed;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Czekaj na otwarcie kasy (jeśli jeszcze nie otwarta)
    int wait_counter = 0;
    while (!cashier_open && !gates_closed && !shutdown_flag && wait_counter < 200) {
        usleep(100000); // 100ms
        wait_counter++;
        
        sem_opusc(g_sem_id, SEM_MAIN);
        cashier_open = g_shm->cashier_open;
        gates_closed = g_shm->gates_closed;
        sem_podnies(g_sem_id, SEM_MAIN);
    }
    
    // Jeśli bramki zamknięte lub nie otwarto kasy w rozsądnym czasie
    if (gates_closed || !cashier_open) {
        return false;
    }
    
    // Zwiększ licznik czekających przy kasie
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->tourists_at_cashier++;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Wyślij prośbę o bilet (VIP używa innego typu)
    if (g_is_vip) {
        msg.mtype = MSG_VIP_PRIORITY + MSG_TOURIST_TO_CASHIER;
    } else {
        msg.mtype = MSG_TOURIST_TO_CASHIER;
    }
    msg.sender_pid = g_pid;
    msg.tourist_id = g_tourist_id;
    msg.age = g_age;
    msg.tourist_type = g_type;
    msg.is_vip = g_is_vip;
    msg.children_count = g_children_count;
    msg.child_ids[0] = g_child_ages[0];
    msg.child_ids[1] = g_child_ages[1];
    
    if (!wyslij_komunikat(g_msg_id, &msg)) {
        sem_opusc(g_sem_id, SEM_MAIN);
        g_shm->tourists_at_cashier--;
        sem_podnies(g_sem_id, SEM_MAIN);
        return false;
    }
    
    // Czekaj na odpowiedź (adresowaną do naszego PID) - z timeoutem 30 sekund
    int timeout_count = 0;
    while (!shutdown_flag && timeout_count < 300) {
        if (odbierz_komunikat_timeout(g_msg_id, &msg, g_pid, 100)) {
            // Dostaliśmy odpowiedź
            break;
        }
        timeout_count++;
    }
    
    // Zmniejsz licznik czekających przy kasie (niezależnie od wyniku)
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->tourists_at_cashier--;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    if (shutdown_flag || timeout_count >= 300) {
        return false;
    }
    
    // Sprawdź czy kasjer nie odmówił (bramki zamknięte)
    if (msg.data == -1) {
        return false;
    }
    
    g_ticket_id = msg.data;
    g_ticket_type = (TicketType)msg.data2;
    
    // Ustaw czas ważności
    time_t now = time(NULL);
    switch (g_ticket_type) {
        case TICKET_TK1:
            g_ticket_valid_until = now + TK1_DURATION;
            break;
        case TICKET_TK2:
            g_ticket_valid_until = now + TK2_DURATION;
            break;
        case TICKET_TK3:
            g_ticket_valid_until = now + TK3_DURATION;
            break;
        case TICKET_SINGLE:
        case TICKET_DAILY:
        default:
            g_ticket_valid_until = 0; // Brak ograniczenia czasowego
            break;
    }
    
    return true;
}

// Sprawdź ważność biletu
bool is_ticket_valid(void) {
    if (g_ticket_type == TICKET_SINGLE) {
        return true; // Jednorazowy - ważny do użycia
    }
    if (g_ticket_type == TICKET_DAILY) {
        return true; // Dzienny - ważny cały dzień
    }
    
    // Czasowe - sprawdź czas
    if (g_ticket_valid_until == 0) {
        return true;
    }
    
    return (time(NULL) <= g_ticket_valid_until);
}

// Przejście przez bramkę wejściową
bool enter_station(void) {
    // Sprawdź czy bramki są otwarte
    sem_opusc(g_sem_id, SEM_MAIN);
    bool gates_closed = g_shm->gates_closed;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    if (gates_closed) {
        logger(LOG_TOURIST, "Turysta #%d - bramki zamknięte, odchodzę", g_tourist_id);
        return false;
    }
    
    // Sprawdź ważność biletu
    if (!is_ticket_valid()) {
        logger(LOG_TOURIST, "Turysta #%d - bilet wygasł! Nie mogę wejść.", g_tourist_id);
        sem_opusc(g_sem_id, SEM_MAIN);
        g_shm->rejected_expired++;
        sem_podnies(g_sem_id, SEM_MAIN);
        return false;
    }
    
    // Czekaj na semafor stacji (limit N osób) - z timeoutem
    int timeout = 0;
    while (sem_probuj_opusc(g_sem_id, SEM_STATION) != 1) {
        if (shutdown_flag || timeout >= 300) {
            return false;
        }
        // Sprawdź czy bramki nie zostały zamknięte w międzyczasie
        sem_opusc(g_sem_id, SEM_MAIN);
        bool gates_now_closed = g_shm->gates_closed;
        bool running = g_shm->is_running;
        sem_podnies(g_sem_id, SEM_MAIN);
        
        if (gates_now_closed) {
            logger(LOG_TOURIST, "Turysta #%d - bramki zamknięte podczas oczekiwania, odchodzę", g_tourist_id);
            return false;
        }
        if (!running) return false;
        
        usleep(100000); // 100ms
        timeout++;
    }
    
    // Jeszcze raz sprawdź bramki po uzyskaniu dostępu do stacji
    sem_opusc(g_sem_id, SEM_MAIN);
    gates_closed = g_shm->gates_closed;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    if (gates_closed) {
        // Bramki zamknięte - zwolnienie semaforu i odejście
        sem_podnies(g_sem_id, SEM_STATION);
        logger(LOG_TOURIST, "Turysta #%d - bramki zamknięte, odchodzę", g_tourist_id);
        return false;
    }
    
    // Czekaj na bramkę wejściową (4 bramki) - z timeoutem
    timeout = 0;
    while (sem_probuj_opusc(g_sem_id, SEM_GATE_ENTRY) != 1) {
        if (shutdown_flag || timeout >= 100) { // max 10 sekund
            sem_podnies(g_sem_id, SEM_STATION); // Zwolnij stację
            return false;
        }
        // Sprawdź czy system jeszcze działa i bramki otwarte
        sem_opusc(g_sem_id, SEM_MAIN);
        bool running = g_shm->is_running;
        bool gates_now_closed = g_shm->gates_closed;
        sem_podnies(g_sem_id, SEM_MAIN);
        if (!running || gates_now_closed) {
            sem_podnies(g_sem_id, SEM_STATION);
            return false;
        }
        usleep(100000);
        timeout++;
    }
    
    // Przydziel numer bramki wejściowej (round-robin)
    sem_opusc(g_sem_id, SEM_MAIN);
    g_entry_gate = (g_tourist_id % ENTRY_GATES) + 1;
    g_shm->tourists_in_station++;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Rejestruj przejście przez bramkę (id karnetu - godzina)
    rejestruj_przejscie_bramki(g_ticket_id);
    
    // Log przejścia przez bramkę wejściową
    const char* vip_str = g_is_vip ? " [VIP]" : "";
    logger(LOG_TOURIST, "Turysta #%d%s wpuszczony przez bramkę wejściową #%d (bilet #%d)",
           g_tourist_id, vip_str, g_entry_gate, g_ticket_id);
    
    // Symulacja przejścia
    usleep(10000 + rand() % 20000); // 10-30ms
    
    // Zwolnij bramkę (semafor stacji po dopiero po wyjściu zwolnie)
    sem_podnies(g_sem_id, SEM_GATE_ENTRY);
    
    logger(LOG_TOURIST, "Turysta #%d%s wszedł na stację dolną (bilet #%d, typ: %s)",
           g_tourist_id, vip_str, g_ticket_id, 
           g_type == TOURIST_CYCLIST ? "rowerzysta" : "pieszy");
    
    return true;
}

// Przejście na peron
bool go_to_platform(void) {
    // Sprawdź czy bramki są zamknięte - jeśli tak turysta na stacji dolnej powinien odejść
    sem_opusc(g_sem_id, SEM_MAIN);
    bool gates_closed = g_shm->gates_closed;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    if (gates_closed) {
        // Bramki zamknięte - opuść stację dolną
        logger(LOG_TOURIST, "Turysta #%d - bramki zamknięte, opuszczam stację dolną", g_tourist_id);
        sem_opusc(g_sem_id, SEM_MAIN);
        g_shm->tourists_in_station--;
        sem_podnies(g_sem_id, SEM_MAIN);
        sem_podnies(g_sem_id, SEM_STATION);
        return false;
    }
    
    // Czekaj na bramkę na peron (3 bramki) - z timeoutem
    int timeout = 0;
    while (sem_probuj_opusc(g_sem_id, SEM_GATE_PLATFORM) != 1) {
        if (shutdown_flag || timeout >= 100) { // max 10 sekund
            // Zwolnij zasoby - opuszczamy stację bez przejścia na peron
            sem_opusc(g_sem_id, SEM_MAIN);
            g_shm->tourists_in_station--;
            sem_podnies(g_sem_id, SEM_MAIN);
            sem_podnies(g_sem_id, SEM_STATION);
            return false;
        }
        // Sprawdź czy bramki nie zostały zamknięte lub system wyłączony
        sem_opusc(g_sem_id, SEM_MAIN);
        bool running = g_shm->is_running;
        gates_closed = g_shm->gates_closed;
        sem_podnies(g_sem_id, SEM_MAIN);
        
        if (gates_closed) {
            // Bramki zamknięte - opuść stację dolną
            logger(LOG_TOURIST, "Turysta #%d - bramki zamknięte podczas oczekiwania na peron, opuszczam stację", g_tourist_id);
            sem_opusc(g_sem_id, SEM_MAIN);
            g_shm->tourists_in_station--;
            sem_podnies(g_sem_id, SEM_MAIN);
            sem_podnies(g_sem_id, SEM_STATION);
            return false;
        }
        if (!running) {
            sem_opusc(g_sem_id, SEM_MAIN);
            g_shm->tourists_in_station--;
            sem_podnies(g_sem_id, SEM_MAIN);
            sem_podnies(g_sem_id, SEM_STATION);
            return false;
        }
        usleep(100000);
        timeout++;
    }
    
    // Wyślij komunikat do worker1
    Message msg;
    msg.mtype = MSG_TOURIST_TO_PLATFORM;
    msg.sender_pid = g_pid;
    msg.tourist_id = g_tourist_id;
    msg.tourist_type = g_type;
    msg.children_count = g_children_count;
    msg.child_ids[0] = 0;
    msg.child_ids[1] = 0;
    
    if (!wyslij_komunikat(g_msg_id, &msg)) {
        sem_podnies(g_sem_id, SEM_GATE_PLATFORM);
        sem_opusc(g_sem_id, SEM_MAIN);
        g_shm->tourists_in_station--;
        sem_podnies(g_sem_id, SEM_MAIN);
        sem_podnies(g_sem_id, SEM_STATION);
        return false;
    }
    
    // Symulacja przejścia przez bramkę
    usleep(5000 + rand() % 10000);
    
    // Zwolnij bramkę na peron
    sem_podnies(g_sem_id, SEM_GATE_PLATFORM);
    
    // Opuściliśmy stację - zwolnij miejsce
    int platform_gate = (g_tourist_id % PLATFORM_GATES) + 1;
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->tourists_in_station--;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    // Zwolnij semafor limitu stacji
    sem_podnies(g_sem_id, SEM_STATION);
    
    logger(LOG_TOURIST, "Turysta #%d przeszedł na peron (bramka peronowa #%d, bilet #%d)", 
           g_tourist_id, platform_gate, g_ticket_id);
    
    return true;
}

// Czekanie na krzesełko i jazda
bool ride_chair(void) {
    Message msg;
    int timeout_count = 0;
    
    // Czekaj na komunikat od worker1 (pozwolenie na wsiadanie)
    // Komunikat jest adresowany do naszego PID
    while (!shutdown_flag && !emergency_flag && timeout_count < 600) {
        if (odbierz_komunikat_timeout(g_msg_id, &msg, g_pid, 100)) {
            if (msg.data == 1) {
                // wsiadanie
                break;
            } else if (msg.data == -1) {
                // Odmowa - system się zamyka
                logger(LOG_TOURIST, "Turysta #%d - odmowa wsiadania (system się zamyka)", g_tourist_id);
                return false;
            }
        }
        
        // Sprawdzenie awarii
        if (emergency_flag) {
            logger(LOG_TOURIST, "Turysta #%d - awaria! Czekam na wznowienie...", g_tourist_id);
            while (emergency_flag && !shutdown_flag) {
                usleep(10000);
            }
            timeout_count = 0; // Reset timeout po awarii
        }
        
        timeout_count++;
    }
    
    if (shutdown_flag || timeout_count >= 600) return false;
    
    logger(LOG_TOURIST, "Turysta #%d wsiada na krzesełko!", g_tourist_id);
    
    // Czekaj na komunikat o dotarciu na górę (data == 2)
    // Timeout: max 30 sekund jazdy
    timeout_count = 0;
    while (!shutdown_flag && timeout_count < 300) {
        if (odbierz_komunikat_timeout(g_msg_id, &msg, g_pid, 100)) {
            if (msg.data == 2) {
                // dotarcie na góre
                break;
            }
        }
        
        if (emergency_flag) {
            logger(LOG_TOURIST, "Turysta #%d - awaria w trakcie jazdy!", g_tourist_id);
            while (emergency_flag && !shutdown_flag) {
                usleep(10000);
            }
            timeout_count = 0; // Reset timeout po awarii
        }
        
        timeout_count++;
    }
    
    return !shutdown_flag && timeout_count < 300;
}

// Opuścić system na górze (dla pieszych)
void exit_at_top(void) {
    logger(LOG_TOURIST, "Turysta #%d (pieszy) opuszcza system na górnej stacji", g_tourist_id);
    
    // Wyślij prośbę o wyjście do worker2 (trasa = -1 oznacza wyjście bez zjazdu)
    Message msg;
    msg.mtype = MSG_TOURIST_EXIT;
    msg.sender_pid = g_pid;
    msg.tourist_id = g_tourist_id;
    msg.data = -1; // Specjalna wartość: wyjście bez zjazdu
    
    wyslij_komunikat(g_msg_id, &msg);
    
    // Czekaj na potwierdzenie wyjścia (z timeoutem)
    int timeout_count = 0;
    while (!shutdown_flag && timeout_count < 100) { // max 10 sekund
        if (odbierz_komunikat_timeout(g_msg_id, &msg, g_pid, 100)) {
            if (msg.data == 3) {
                break;
            }
        }
        timeout_count++;
    }
    
    // Rejestruj "zjazd" (właściwie wyjście na górze) dla pieszego - WEWNĄTRZ sekcji krytycznej
    sem_opusc(g_sem_id, SEM_MAIN);
    if (g_ticket_id > 0 && g_ticket_id < MAX_TICKETS) {
        g_shm->ticket_rides[g_ticket_id]++;
    }
    sem_podnies(g_sem_id, SEM_MAIN);
    
    logger(LOG_TOURIST, "Turysta #%d (pieszy) zakończył wizytę na górnej stacji (bilet #%d)", 
           g_tourist_id, g_ticket_id);
}

// Zjazd trasą i powrót na stację dolną (dla rowerzystów)
void descend_trail(void) {
    // Wybierz trasę (losowo z wagami)
    TrailType trail;
    int r = rand() % 100;
    if (r < 40) {
        trail = TRAIL_T1; // 40% łatwa
    } else if (r < 75) {
        trail = TRAIL_T2; // 35% średnia
    } else {
        trail = TRAIL_T3; // 25% trudna
    }
    
    const char* trail_names[] = {"T1 (łatwa)", "T2 (średnia)", "T3 (trudna)"};
    logger(LOG_TOURIST, "Turysta #%d wybiera trasę zjazdową %s", 
           g_tourist_id, trail_names[trail]);
    
    // Wyślij prośbę o wyjście do worker2
    Message msg;
    msg.mtype = MSG_TOURIST_EXIT;
    msg.sender_pid = g_pid;
    msg.tourist_id = g_tourist_id;
    msg.data = trail;
    
    wyslij_komunikat(g_msg_id, &msg);
    
    // Czekaj na potwierdzenie zjazdu (worker2 symuluje zjazd, data == 3)
    int timeout_count = 0;
    while (!shutdown_flag && timeout_count < 100) { // max 10 sekund
        if (odbierz_komunikat_timeout(g_msg_id, &msg, g_pid, 100)) {
            if (msg.data == 3) {
                // Zjazd zakończony
                break;
            }
        }
        timeout_count++;
    }
    
    // Rejestruj zjazd dla tego biletu - WEWNĄTRZ sekcji krytycznej
    sem_opusc(g_sem_id, SEM_MAIN);
    if (g_ticket_id > 0 && g_ticket_id < MAX_TICKETS) {
        g_shm->ticket_rides[g_ticket_id]++;
        g_shm->trail_usage[trail]++;
    }
    sem_podnies(g_sem_id, SEM_MAIN);
    
    logger(LOG_TOURIST, "Turysta #%d zakończył trasę zjazdową i dotarł na stację dolną (bilet #%d)", 
           g_tourist_id, g_ticket_id);
}

// Obsługa wielokrotnych przejazdów (dla biletów czasowych i dziennych)
bool can_ride_again(void) {
    if (g_ticket_type == TICKET_SINGLE) {
        return false; // Jednorazowy - tylko jeden przejazd
    }
    
    if (!is_ticket_valid()) {
        logger(LOG_TOURIST, "Turysta #%d - bilet czasowy wygasł!", g_tourist_id);
        return false;
    }
    
    // Sprawdź czy bramki są otwarte
    sem_opusc(g_sem_id, SEM_MAIN);
    bool gates_closed = g_shm->gates_closed;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    if (gates_closed) {
        return false;
    }
    
    // Losowa szansa na kolejny przejazd (50%)
    return (rand() % 100 < 50);
}

int main(int argc, char* argv[]) {
    // Inicjalizacja loggera dla procesu potomnego
    logger_init_child();
    
    // Konfiguracja sygnałów
    struct sigaction sa;
    sa.sa_handler = tourist_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    
    g_pid = getpid();
    srand(time(NULL) ^ g_pid);
    
    // Parsuj argumenty
    if (argc < 2) {
        fprintf(stderr, "Użycie: %s <tourist_id> [age] [type] [is_vip] [children_count]\n", argv[0]);
        return 1;
    }
    
    g_tourist_id = atoi(argv[1]);
    g_age = (argc > 2) ? atoi(argv[2]) : (18 + rand() % 50);
    g_type = (argc > 3) ? (TouristType)atoi(argv[3]) : (TouristType)(rand() % 2);
    g_is_vip = (argc > 4) ? (atoi(argv[4]) != 0) : false;
    g_children_count = (argc > 5) ? atoi(argv[5]) : 0;
    
    // Ogranicz dzieci do max 2
    if (g_children_count > 2) g_children_count = 2;
    
    // Wygeneruj wiek dzieci (4-8 lat - wymagają opieki)
    for (int i = 0; i < g_children_count; i++) {
        g_child_ages[i] = 4 + rand() % 5; // 4-8 lat
    }
    
    // Połącz z zasobami IPC
    g_msg_id = polacz_kolejke();
    g_sem_id = polacz_semafory();
    int shm_id = polacz_pamiec();
    g_shm = dolacz_pamiec(shm_id);
    
    // Sprawdź czy symulacja jeszcze trwa
    sem_opusc(g_sem_id, SEM_MAIN);
    bool is_running = g_shm->is_running;
    bool gates_closed = g_shm->gates_closed;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    if (!is_running || gates_closed) {
        sem_opusc(g_sem_id, SEM_MAIN);
        g_shm->total_tourists_finished++;
        sem_podnies(g_sem_id, SEM_MAIN);
        odlacz_pamiec(g_shm);
        return 0;
    }
    
    // Uruchom wątki dzieci
    if (g_children_count > 0) {
        start_children_threads();
    }
    
    const char* type_str = g_type == TOURIST_CYCLIST ? "rowerzysta" : "pieszy";
    const char* vip_str = g_is_vip ? " [VIP]" : "";
    
    if (g_children_count > 0) {
        logger(LOG_TOURIST, "Turysta #%d%s przybywa (%s, %d lat, %d dzieci pod opieką)",
               g_tourist_id, vip_str, type_str, g_age, g_children_count);
    } else {
        logger(LOG_TOURIST, "Turysta #%d%s przybywa (%s, %d lat)",
               g_tourist_id, vip_str, type_str, g_age);
    }
    
    // Niektórzy turyści nie korzystają z kolei (5%)
    if (rand() % 100 < 5) {
        logger(LOG_TOURIST, "Turysta #%d tylko ogląda i odchodzi", g_tourist_id);
        
        if (g_children_count > 0) {
            finish_children_threads();
        }
        
        sem_opusc(g_sem_id, SEM_MAIN);
        g_shm->total_tourists_finished++;
        sem_podnies(g_sem_id, SEM_MAIN);
        
        odlacz_pamiec(g_shm);
        return 0;
    }
    
    // 1. Kupno biletu
    if (!buy_ticket()) {
        logger(LOG_TOURIST, "Turysta #%d nie mógł kupić biletu - rezygnuje", g_tourist_id);
        
        if (g_children_count > 0) {
            finish_children_threads();
        }
        
        sem_opusc(g_sem_id, SEM_MAIN);
        g_shm->total_tourists_finished++;
        sem_podnies(g_sem_id, SEM_MAIN);
        
        odlacz_pamiec(g_shm);
        return 0;
    }
    
    int ride_count = 0;
    
    do {
        // 2. Wejście na stację
        if (!enter_station()) {
            break;
        }
        
        // 3. Przejście na peron
        if (!go_to_platform()) {
            break;
        }
        
        // 4. Jazda krzesełkiem
        if (!ride_chair()) {
            break;
        }
        
        ride_count++;
        
        // 5. Piesi opuszczają system na górze, rowerzyści zjeżdżają trasą
        if (g_type == TOURIST_PEDESTRIAN) {
            exit_at_top();
            break; // Pieszy kończy po jednym przejeździe
        } else {
            // Rowerzysta zjeżdża trasą
            descend_trail();
        }
        
        // Dla biletów jednorazowych - koniec
        if (g_ticket_type == TICKET_SINGLE) {
            break;
        }
        
    } while (can_ride_again() && !shutdown_flag);
    
    // Zakończ wątki dzieci
    if (g_children_count > 0) {
        finish_children_threads();
    }
    
    logger(LOG_TOURIST, "Turysta #%d kończy wizytę (przejazdy: %d)", g_tourist_id, ride_count);
    
    // Aktualizuj licznik
    sem_opusc(g_sem_id, SEM_MAIN);
    g_shm->total_tourists_finished++;
    sem_podnies(g_sem_id, SEM_MAIN);
    
    odlacz_pamiec(g_shm);
    return 0;
}
