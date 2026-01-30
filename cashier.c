// cashier.c - proces kasjera (sprzedaż biletów, obsługa bramek wejściowych)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/msg.h>
#include "struktury.h"
#include "operacje.h"
#include "logger.h"

// Flagi sygnałów
static volatile sig_atomic_t shutdown_flag = 0;
static volatile sig_atomic_t emergency_flag = 0;

// Handler sygnałów
void cashier_signal_handler(int sig) {
    if (sig == SIGTERM) {
        shutdown_flag = 1;
    } else if (sig == SIGUSR1) {
        emergency_flag = 1;
    } else if (sig == SIGUSR2) {
        emergency_flag = 0; // Wznowienie po awarii
    }
}

// Struktura turysty w kolejce
typedef struct {
    pid_t pid;
    int tourist_id;
    int age;
    TouristType type;
    bool is_vip;
    int children_count;
    int child_ids[2];
    TicketType ticket_type;
} QueuedTourist;

// Kolejka priorytetowa (VIP na początku)
#define MAX_QUEUE 15000
static QueuedTourist normal_queue[MAX_QUEUE];
static int normal_queue_size = 0;
static QueuedTourist vip_queue[MAX_QUEUE];
static int vip_queue_size = 0;

// Dodawanie do kolejki - zwraca true jeśli dodano, false jeśli kolejka pełna
bool add_to_queue(QueuedTourist* tourist) {
    if (tourist->is_vip) {
        if (vip_queue_size < MAX_QUEUE) {
            vip_queue[vip_queue_size++] = *tourist;
            return true;
        }
    } else {
        if (normal_queue_size < MAX_QUEUE) {
            normal_queue[normal_queue_size++] = *tourist;
            return true;
        }
    }
    return false;
}

// Pobieranie z kolejki (VIP ma priorytet)
bool get_from_queue(QueuedTourist* tourist) {
    if (vip_queue_size > 0) {
        *tourist = vip_queue[0];
        // Przesunięcie kolejki
        for (int i = 0; i < vip_queue_size - 1; i++) {
            vip_queue[i] = vip_queue[i + 1];
        }
        vip_queue_size--;
        return true;
    }
    
    if (normal_queue_size > 0) {
        *tourist = normal_queue[0];
        for (int i = 0; i < normal_queue_size - 1; i++) {
            normal_queue[i] = normal_queue[i + 1];
        }
        normal_queue_size--;
        return true;
    }
    
    return false;
}

int main(void) {
    // Inicjalizacja loggera dla procesu potomnego
    logger_init_child();
    
    // Konfiguracja sygnałów
    struct sigaction sa;
    sa.sa_handler = cashier_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    
    // Połączenie z zasobami IPC
    int msg_id = polacz_kolejke();
    int sem_id = polacz_semafory();
    int shm_id = polacz_pamiec();
    SharedMemory* shm = dolacz_pamiec(shm_id);
    
    srand(time(NULL) ^ getpid());
    
    // Pobranie czasu rozpoczęcia symulacji
    sem_opusc(sem_id, SEM_MAIN);
    time_t sim_start = shm->simulation_start;
    sem_podnies(sem_id, SEM_MAIN);
    
    // Opóźnienie rozpoczęcia pracy kasjera o WORK_START_TIME sekund
    logger(LOG_CASHIER, "Czekam %d sekund przed rozpoczęciem pracy...", WORK_START_TIME);
    
    while (!shutdown_flag) {
        time_t now = time(NULL);
        time_t elapsed = now - sim_start;
        
        if (elapsed >= WORK_START_TIME) {
            break;
        }
    }
    
    if (shutdown_flag) {
        logger(LOG_CASHIER, "Przerwano przed rozpoczęciem pracy");
        odlacz_pamiec(shm);
        return 0;
    }
    
    // Ustawienie flagi że kasa jest otwarta
    sem_opusc(sem_id, SEM_MAIN);
    shm->cashier_open = true;
    sem_podnies(sem_id, SEM_MAIN);
    
    logger(LOG_CASHIER, "Rozpoczynam pracę - kasa otwarta!");
    
    Message msg;
    
    while (!shutdown_flag) {
        // Sprawdzenie awarii
        if (emergency_flag) {
            logger(LOG_CASHIER, "AWARIA - wstrzymuję sprzedaż biletów!");
            while (emergency_flag && !shutdown_flag) {
                // Aktywne czekanie na wznowienie
            }
            if (!shutdown_flag) {
                logger(LOG_CASHIER, "Wznawiam sprzedaż biletów");
            }
            continue;
        }
        
        // Sprawdzenie czy bramki są zamknięte (koniec dnia)
        sem_opusc(sem_id, SEM_MAIN);
        bool gates_closed = shm->gates_closed;
        sem_podnies(sem_id, SEM_MAIN);
        
        // Gdy bramki zamknięte - opróżnienie kolejek i odrzucenie wszystkich czekających
        if (gates_closed) {
            // Opróżnij kolejkę VIP komunikatów
            while (odbierz_komunikat(msg_id, &msg, MSG_VIP_PRIORITY + MSG_TOURIST_TO_CASHIER, false)) {
                // Wysłanie odmowy
                Message response;
                response.mtype = msg.sender_pid;
                response.sender_pid = getpid();
                response.tourist_id = msg.tourist_id;
                response.data = -1; // Odmowa
                response.data2 = -1;
                wyslij_komunikat(msg_id, &response);
                logger(LOG_CASHIER, "Bramki zamknięte - odmowa dla VIP #%d", msg.tourist_id);
            }
            
            // Opróżnij kolejkę zwykłych komunikatów
            while (odbierz_komunikat(msg_id, &msg, MSG_TOURIST_TO_CASHIER, false)) {
                Message response;
                response.mtype = msg.sender_pid;
                response.sender_pid = getpid();
                response.tourist_id = msg.tourist_id;
                response.data = -1;
                response.data2 = -1;
                wyslij_komunikat(msg_id, &response);
                logger(LOG_CASHIER, "Bramki zamknięte - odmowa dla turysty #%d", msg.tourist_id);
            }
            
            // Opróżnij wewnętrzną kolejkę - odrzuć tych co czekają
            QueuedTourist qt;
            while (get_from_queue(&qt)) {
                Message response;
                response.mtype = qt.pid;
                response.sender_pid = getpid();
                response.tourist_id = qt.tourist_id;
                response.data = -1;
                response.data2 = -1;
                wyslij_komunikat(msg_id, &response);
                logger(LOG_CASHIER, "Bramki zamknięte - odmowa dla turysty #%d (z kolejki wewnętrznej)", qt.tourist_id);
            }
            
            // Aktywne czekanie gdy bramki zamknięte
            continue;
        }
        
        // Odbieranie komunikatów od turystów chcących kupić bilet
        // Najpierw VIP (mtype = MSG_VIP_PRIORITY + MSG_TOURIST_TO_CASHIER)
        while (odbierz_komunikat(msg_id, &msg, MSG_VIP_PRIORITY + MSG_TOURIST_TO_CASHIER, false)) {
            if (shutdown_flag || gates_closed) break;
            
            QueuedTourist qt;
            qt.pid = msg.sender_pid;
            qt.tourist_id = msg.tourist_id;
            qt.age = msg.age;
            qt.type = msg.tourist_type;
            qt.is_vip = true;
            qt.children_count = msg.children_count;
            qt.child_ids[0] = msg.child_ids[0];
            qt.child_ids[1] = msg.child_ids[1];
            qt.ticket_type = msg.ticket_type;
            
            if (add_to_queue(&qt)) {
                logger(LOG_VIP, "VIP #%d dołączył do kolejki priorytetowej!", qt.tourist_id);
            } else {
                // Kolejka pełna - wyślij odmowę
                Message response;
                response.mtype = qt.pid;
                response.sender_pid = getpid();
                response.tourist_id = qt.tourist_id;
                response.data = -1;
                response.data2 = -1;
                wyslij_komunikat(msg_id, &response);
                logger(LOG_CASHIER, "Kolejka pełna - odmowa dla VIP #%d", qt.tourist_id);
            }
        }
        
        // zwykli turyści
        while (odbierz_komunikat(msg_id, &msg, MSG_TOURIST_TO_CASHIER, false)) {
            if (shutdown_flag || gates_closed) break;
            
            QueuedTourist qt;
            qt.pid = msg.sender_pid;
            qt.tourist_id = msg.tourist_id;
            qt.age = msg.age;
            qt.type = msg.tourist_type;
            qt.is_vip = false;
            qt.children_count = msg.children_count;
            qt.child_ids[0] = msg.child_ids[0];
            qt.child_ids[1] = msg.child_ids[1];
            qt.ticket_type = msg.ticket_type;
            
            if (!add_to_queue(&qt)) {
                // Kolejka pełna - wyślij odmowę
                Message response;
                response.mtype = qt.pid;
                response.sender_pid = getpid();
                response.tourist_id = qt.tourist_id;
                response.data = -1;
                response.data2 = -1;
                wyslij_komunikat(msg_id, &response);
                logger(LOG_CASHIER, "Kolejka pełna - odmowa dla turysty #%d", qt.tourist_id);
            }
        }
        
        // Obsługa klientów z kolejki
        QueuedTourist tourist;
        while (get_from_queue(&tourist) && !shutdown_flag && !emergency_flag) {
            // Typ biletu z żądania turysty
            TicketType ticket_type = tourist.ticket_type;
            
            // Sprawdzanie zniżki (dzieci <10, seniorzy >65)
            bool has_discount = (tourist.age < 10 || tourist.age > 65);
            int price = cena_biletu(ticket_type, has_discount);
            
            // Aktualizacja statystyk
            sem_opusc(sem_id, SEM_MAIN);
            shm->tickets_sold[ticket_type]++;
            shm->total_revenue += price;
            int ticket_id = ++shm->next_ticket_id;
            
            if (tourist.is_vip) {
                shm->vip_served++;
            }
            
            // Dzieci z opiekunem
            if (tourist.children_count > 0) {
                shm->children_with_guardian += tourist.children_count;
                for (int i = 0; i < tourist.children_count; i++) {
                    shm->tickets_sold[ticket_type]++;
                    int child_price = cena_biletu(ticket_type, true); // zniżka dla dziecka
                    shm->total_revenue += child_price;
                }
            }
            sem_podnies(sem_id, SEM_MAIN);
            
            // Wysłanie potwierdzenia do turysty 
            Message response;
            response.mtype = tourist.pid; // Adresowanie do konkretnego turysty
            response.sender_pid = getpid();
            response.tourist_id = tourist.tourist_id;
            response.data = ticket_id;
            response.data2 = ticket_type;
            
            if (wyslij_komunikat(msg_id, &response)) {
                const char* ticket_name = nazwa_biletu(ticket_type);
                const char* discount_str = has_discount ? " (ze zniżką 25%)" : "";
                const char* vip_str = tourist.is_vip ? " [VIP]" : "";
                const char* type_str = tourist.type == TOURIST_CYCLIST ? "rowerzysta" : "pieszy";
                
                logger(LOG_CASHIER, "Sprzedano bilet #%d (%s) turyscie #%d (%s, %d lat)%s%s - cena: %d",
                       ticket_id, ticket_name, tourist.tourist_id, type_str, 
                       tourist.age, discount_str, vip_str, price);
                
                if (tourist.children_count > 0) {
                    logger(LOG_CASHIER, "  -> Turysta #%d ma pod opieką %d dzieci", 
                           tourist.tourist_id, tourist.children_count);
                }
            }
        }
        
    }
    
    logger(LOG_CASHIER, "Zamykam kasę - koniec pracy!");
    
    odlacz_pamiec(shm);
    return 0;
}
