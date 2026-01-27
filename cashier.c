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
} QueuedTourist;

// Kolejka priorytetowa (VIP na początku)
#define MAX_QUEUE 1000
static QueuedTourist normal_queue[MAX_QUEUE];
static int normal_queue_size = 0;
static QueuedTourist vip_queue[MAX_QUEUE];
static int vip_queue_size = 0;

// Dodawanie do kolejki
void add_to_queue(QueuedTourist* tourist) {
    if (tourist->is_vip) {
        if (vip_queue_size < MAX_QUEUE) {
            vip_queue[vip_queue_size++] = *tourist;
        }
    } else {
        if (normal_queue_size < MAX_QUEUE) {
            normal_queue[normal_queue_size++] = *tourist;
        }
    }
}

// Pobieranie z kolejki (VIP ma priorytet)
bool get_from_queue(QueuedTourist* tourist) {
    if (vip_queue_size > 0) {
        *tourist = vip_queue[0];
        // Przesuń kolejkę
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

bool queue_empty(void) {
    return (vip_queue_size == 0 && normal_queue_size == 0);
}

int main(void) {
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
    
    logger(LOG_CASHIER, "Rozpoczynam pracę - kasa otwarta!");
    
    Message msg;
    
    while (!shutdown_flag) {
        // Sprawdzenie awarii
        if (emergency_flag) {
            logger(LOG_CASHIER, "AWARIA - wstrzymuję sprzedaż biletów!");
            while (emergency_flag && !shutdown_flag) {
                usleep(10000); // Czekaj na wznowienie
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
        
        if (gates_closed) {
            // Nie przyjmowanie nowych klientów
            if (queue_empty()) {
                logger(LOG_CASHIER, "Bramki zamknięte, kolejka pusta - kończę pracę");
                break;
            }
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
            
            add_to_queue(&qt);
            logger(LOG_VIP, "VIP #%d dołączył do kolejki priorytetowej!", qt.tourist_id);
        }
        
        // Potem zwykli turyści
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
            
            add_to_queue(&qt);
        }
        
        // Obsługa klientów z kolejki
        QueuedTourist tourist;
        while (get_from_queue(&tourist) && !shutdown_flag && !emergency_flag) {
            // Losowy typ biletu
            TicketType ticket_type = rand() % TICKET_TYPE_COUNT;
            
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
        
        // Krótka pauza jeśli kolejka pusta
        if (queue_empty()) {
            usleep(1000); // 1ms
        }
    }
    
    logger(LOG_CASHIER, "Zamykam kasę - koniec pracy!");
    
    odlacz_pamiec(shm);
    return 0;
}
