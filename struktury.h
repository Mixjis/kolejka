#ifndef STRUKTURY_H
#define STRUKTURY_H

#include <pthread.h>
#include <sys/types.h>
#include <stdbool.h>
#include <time.h>

// ==== KONFIGURACJA SYMULACJI ====
#define TOTAL_TOURISTS       10000   // Liczba turystów do obsłużenia
#define MAX_CHAIRS           72      // Łączna liczba krzesełek
#define MAX_ACTIVE_CHAIRS    36      // Maks krzesełek jednocześnie w ruchu
#define CHAIR_CAPACITY       4       // Pojemność jednego krzesełka

#define STATION_CAPACITY     50      // N - max osób na dolnej stacji (poczekalni)
#define ENTRY_GATES          4       // Bramki wejściowe (kontrola biletów)
#define PLATFORM_GATES       3       // Bramki na peron (kontrola grup)
#define EXIT_GATES           2       // Wyjścia ze stacji górnej

// Czasy przejazdu trasami (sekundy symulacyjne)
#define TRAIL_T1_TIME        1       // Łatwa
#define TRAIL_T2_TIME        2       // Średnia
#define TRAIL_T3_TIME        3       // Trudna
#define CHAIR_TRAVEL_TIME    2       // Czas przejazdu krzesełka

// Godziny pracy (w sekundach symulacyjnych od startu)
#define WORK_START_TIME      0       // Tp - start
#define WORK_END_TIME        15     // Tk - koniec (sekundy)
#define SHUTDOWN_DELAY       3       // Opóźnienie przed wyłączeniem po Tk

// Szybkość generowania turystów
#define TOURIST_SPAWN_DELAY_MAX  2000  // opóźnienie między turystami

// Procent VIPów
#define VIP_PERCENT          1

// ==== TYPY BILETÓW ====
typedef enum {
    TICKET_SINGLE = 0,   // Jednorazowy
    TICKET_TK1,          // Czasowy 1h (w symulacji: 15s)
    TICKET_TK2,          // Czasowy 2h (w symulacji: 30s)
    TICKET_TK3,          // Czasowy 3h (w symulacji: 45s)
    TICKET_DAILY,        // Dzienny
    TICKET_TYPE_COUNT
} TicketType;

// Czasy ważności karnetów czasowych (sekundy symulacyjne)
#define TK1_DURATION         15
#define TK2_DURATION         30
#define TK3_DURATION         45

// Ceny biletów (jednostki)
#define PRICE_SINGLE         10
#define PRICE_TK1            25
#define PRICE_TK2            40
#define PRICE_TK3            55
#define PRICE_DAILY          80
#define DISCOUNT_PERCENT     25   // Zniżka dla dzieci <10 i seniorów >65

// ==== TYPY TURYSTÓW ====
typedef enum {
    TOURIST_PEDESTRIAN = 0,  // Pieszy
    TOURIST_CYCLIST          // Rowerzysta
} TouristType;

// ==== TRASY ZJAZDOWE ====
typedef enum {
    TRAIL_T1 = 0,    // Łatwa
    TRAIL_T2,        // Średnia
    TRAIL_T3,        // Trudna
    TRAIL_COUNT
} TrailType;

// ==== TYPY KOMUNIKATÓW ====
#define MSG_TOURIST_TO_CASHIER    1    // Turysta -> Kasjer (kupno biletu)
#define MSG_CASHIER_TO_TOURIST    2    // Kasjer -> Turysta (potwierdzenie)
#define MSG_TOURIST_TO_GATE       3    // Turysta -> Bramka (wejście)
#define MSG_GATE_TO_TOURIST       4    // Bramka -> Turysta (przepuszczenie)
#define MSG_TOURIST_TO_PLATFORM   5    // Turysta -> Peron
#define MSG_PLATFORM_TO_TOURIST   6    // Peron -> Turysta
#define MSG_WORKER_EMERGENCY      7    // Awaryjne zatrzymanie
#define MSG_WORKER_READY          8    // Gotowość do wznowienia
#define MSG_CHAIR_DEPARTURE       9    // Odjazd krzesełka
#define MSG_CHAIR_ARRIVAL         10   // Przyjazd krzesełka
#define MSG_TOURIST_EXIT          11   // Turysta opuszcza górną stację
#define MSG_VIP_PRIORITY          100  // Priorytet VIP (offset)

// ==== KLUCZE IPC ====
#define IPC_KEY_PATH           "."
#define IPC_KEY_SEM            'S'
#define IPC_KEY_SHM            'M'
#define IPC_KEY_MSG            'Q'
#define IPC_KEY_MSG_WORKER     'W'

// ==== INDEKSY SEMAFORÓW ====
#define SEM_MAIN               0    // Główny mutex pamięci dzielonej
#define SEM_STATION            1    // Limit osób na stacji (N)
#define SEM_PLATFORM           2    // Dostęp do peronu
#define SEM_CHAIRS             3    // Liczba dostępnych krzesełek
#define SEM_GATE_ENTRY         4    // Bramki wejściowe
#define SEM_GATE_PLATFORM      5    // Bramki na peron
#define SEM_GATE_EXIT          6    // Wyjścia górna stacja
#define SEM_EMERGENCY          7    // Awaryjne zatrzymanie
#define SEM_WORKER_SYNC        8    // Synchronizacja pracowników
#define SEM_LOG_FILE           9    // Mutex dla pliku logów
#define SEM_REPORT             10   // Mutex dla raportu
#define SEM_COUNT              11   // Liczba semaforów

// ==== STRUKTURY DANYCH ====

// Bilet/Karnet
typedef struct {
    int id;
    TicketType type;
    time_t purchase_time;
    time_t valid_until;     // 0 - dzienny
    int rides_count;        // Liczba przejazdów
    bool is_vip;
    int owner_age;
    bool has_discount;
} Ticket;

// Przejście przez bramkę
typedef struct {
    int ticket_id;
    time_t timestamp;
    int gate_number;
} GatePassage;

// Turysta w pamięci dzielonej
typedef struct {
    pid_t pid;
    int id;
    int age;
    TouristType type;           // Pieszy/rowerzysta
    Ticket ticket;
    bool is_vip;
    bool is_child_with_guardian; // Dziecko <8 lat z opiekunem
    int guardian_id;            // ID opiekuna (-1 jeśli brak)
    int children_count;         // Ile dzieci pod opieką (dla dorosłych)
    int child_ids[2];           // ID dzieci pod opieką (max 2)
    bool on_platform;
    bool on_chair;
    int assigned_chair;
    TrailType chosen_trail;
} Tourist;

// Krzesełko
typedef struct {
    int id;
    int passengers[CHAIR_CAPACITY];  // ID turystów (-1 = puste)
    int passenger_count;
    int cyclist_count;
    int pedestrian_count;
    bool in_transit;
    time_t departure_time;
} Chair;

// Pamięć dzielona
typedef struct {
    // Stan systemu
    bool is_running;
    bool emergency_stop;
    bool gates_closed;          // Tk osiągnięte - bramki zamknięte
    time_t simulation_start;
    time_t simulation_end;
    
    // Statystyki sprzedaży
    int tickets_sold[TICKET_TYPE_COUNT];
    int total_revenue;
    
    // Statystyki przejazdów
    int chair_departures;
    int passengers_transported;
    int cyclists_transported;
    int pedestrians_transported;
    
    // Kategorie specjalne
    int vip_served;
    int children_with_guardian;
    int rejected_expired;       // Odrzuceni z wygasłym karnetem
    
    // Trasy
    int trail_usage[TRAIL_COUNT];
    
    // Przejścia bramkowe - historia
    int gate_passages_count;
    #define MAX_GATE_PASSAGES 20000
    GatePassage gate_passages[MAX_GATE_PASSAGES];
    
    // Statystyki per bilet (liczba zjazdów)
    #define MAX_TICKETS 20000
    int ticket_rides[MAX_TICKETS];  // ticket_rides[ticket_id] = liczba zjazdów
    
    // Kolejki i liczniki
    int tourists_in_station;    // Na dolnej stacji
    int tourists_on_platform;   // Na peronie (dolna stacja)
    int tourists_at_top;        // Na górnej stacji (czekający na wyjście/zjazd)
    int active_chairs;          // Krzesełka w ruchu
    int tourists_waiting_entry; // Czekający przed bramkami
    int tourists_at_cashier;    // Czekający na bilet przy kasie
    int tourists_descending;    // W trakcie zjazdu trasą
    
    // PIDy procesów
    pid_t main_pid;
    pid_t cashier_pid;
    pid_t worker1_pid;
    pid_t worker2_pid;
    
    // Flagi awaryjne
    int emergency_initiator;    // 1 lub 2 (który pracownik)
    bool worker1_ready;
    bool worker2_ready;
    
    // Kolejny ID
    int next_tourist_id;
    int next_ticket_id;
    int next_chair_id;
    
    // Liczniki do zakończenia
    int total_tourists_created;
    int total_tourists_finished;
    
    // Krzesełka
    Chair chairs[MAX_CHAIRS];
    
    // VIP queue offset
    int vip_queue_size;
} SharedMemory;

// Struktura komunikatu
typedef struct {
    long mtype;
    pid_t sender_pid;
    int tourist_id;
    int data;           // Różne dane w zależności od typu
    int data2;          // Dodatkowe dane
    TouristType tourist_type;
    int age;
    bool is_vip;
    int children_count;
    int child_ids[CHAIR_CAPACITY];
} Message;

#define MSG_SIZE (sizeof(Message) - sizeof(long))

// ==== KOLORY ANSI ====
#define ANSI_RESET       "\033[0m"
#define ANSI_RED         "\033[31m"
#define ANSI_GREEN       "\033[32m"
#define ANSI_YELLOW      "\033[33m"
#define ANSI_BLUE        "\033[34m"
#define ANSI_MAGENTA     "\033[35m"
#define ANSI_CYAN        "\033[36m"
#define ANSI_WHITE       "\033[37m"
#define ANSI_BOLD        "\033[1m"
#define ANSI_BRIGHT_RED  "\033[91m"
#define ANSI_BRIGHT_GREEN "\033[92m"
#define ANSI_BRIGHT_YELLOW "\033[93m"
#define ANSI_BRIGHT_BLUE "\033[94m"
#define ANSI_BRIGHT_MAGENTA "\033[95m"
#define ANSI_BRIGHT_CYAN "\033[96m"

#endif // STRUKTURY_H
