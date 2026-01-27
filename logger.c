// logger.c - kolorowe logi ANSI, sekwencyjny zapis do pliku

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include "logger.h"
#include "struktury.h"
#include "operacje.h"

#define LOG_FILE "kolej_log.txt"
#define REPORT_FILE "raport_karnetow.txt"

static FILE* file_log = NULL;
static FILE* file_report = NULL;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static int log_sequence = 0;

// Pobierz kolor dla typu nadawcy
static const char* get_color(LogSender sender) {
    switch (sender) {
        case LOG_SYSTEM:    return ANSI_WHITE;
        case LOG_CASHIER:   return ANSI_GREEN;
        case LOG_WORKER1:   return ANSI_YELLOW;
        case LOG_WORKER2:   return ANSI_BLUE;
        case LOG_TOURIST:   return ANSI_CYAN;
        case LOG_VIP:       return ANSI_BRIGHT_MAGENTA;
        case LOG_CHAIR:     return ANSI_MAGENTA;
        case LOG_EMERGENCY: return ANSI_BRIGHT_RED;
        case LOG_REPORT:    return ANSI_BRIGHT_GREEN;
        default:            return ANSI_RESET;
    }
}

// Pobierz nazwę nadawcy
static const char* get_sender_name(LogSender sender) {
    switch (sender) {
        case LOG_SYSTEM:    return "SYSTEM";
        case LOG_CASHIER:   return "KASJER";
        case LOG_WORKER1:   return "PRACOWNIK1";
        case LOG_WORKER2:   return "PRACOWNIK2";
        case LOG_TOURIST:   return "TURYSTA";
        case LOG_VIP:       return "VIP";
        case LOG_CHAIR:     return "KRZESEŁKO";
        case LOG_EMERGENCY: return "AWARIA";
        case LOG_REPORT:    return "RAPORT";
        default:            return "???";
    }
}

static int safe_write(int fd, const char* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t ret = write(fd, data + written, len - written);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += ret;
    }
    return 0;
}

// Pobierz timestamp
static void get_timestamp(char* buffer, size_t size) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm* tm_info = localtime(&tv.tv_sec);
    
    snprintf(buffer, size, "%02d:%02d:%02d.%03ld",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             tv.tv_usec / 1000);
}

void logger_init(void) {
    pthread_mutex_lock(&file_mutex);
    
    if (file_log == NULL) {
        file_log = fopen(LOG_FILE, "w");
        if (file_log == NULL) {
            perror("Nie można otworzyć pliku logów");
        } else {
            setvbuf(file_log, NULL, _IOLBF, 0);
        }
    }
    
    if (file_report == NULL) {
        file_report = fopen(REPORT_FILE, "w");
        if (file_report == NULL) {
            perror("Nie można otworzyć pliku raportu");
        }
    }
    
    pthread_mutex_unlock(&file_mutex);
    
    // Nagłówek pliku logów
    if (file_log != NULL) {
        const char* header = "========== START SYMULACJI KOLEI LINOWEJ ==========\n";
        fprintf(file_log, "%s", header);
        fflush(file_log);
    }
}

void logger_init_child(void) {
    // Procesy potomne muszą ponownie otworzyć pliki
    pthread_mutex_lock(&file_mutex);
    
    // Zamknij stare deskryptory jeśli istnieją
    if (file_log != NULL) {
        fclose(file_log);
        file_log = NULL;
    }
    if (file_report != NULL) {
        fclose(file_report);
        file_report = NULL;
    }
    
    // Otwórz ponownie w trybie append
    file_log = fopen(LOG_FILE, "a");
    if (file_log == NULL) {
        perror("Nie można otworzyć pliku logów (child)");
    } else {
        // Wyłącz pełne buforowanie - używaj buforowania linii
        setvbuf(file_log, NULL, _IOLBF, 0);
    }
    
    file_report = fopen(REPORT_FILE, "a");
    if (file_report == NULL) {
        perror("Nie można otworzyć pliku raportu (child)");
    }
    
    pthread_mutex_unlock(&file_mutex);
}

void logger_close(void) {
    pthread_mutex_lock(&file_mutex);
    
    if (file_log != NULL) {
        const char* footer = "========== KONIEC SYMULACJI ==========\n";
        fprintf(file_log, "%s", footer);
        fflush(file_log);
        fclose(file_log);
        file_log = NULL;
    }
    
    if (file_report != NULL) {
        fclose(file_report);
        file_report = NULL;
    }
    
    pthread_mutex_unlock(&file_mutex);
}

void logger(LogSender sender, const char* format, ...) {
    char timestamp[32];
    char message[1024];
    char console_line[2048];
    char file_line[2048];
    
    get_timestamp(timestamp, sizeof(timestamp));
    
    // Formatowanie
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    pid_t pid = getpid();
    const char* color = get_color(sender);
    const char* name = get_sender_name(sender);
    
    // Linia do konsoli (z kolorami)
    snprintf(console_line, sizeof(console_line),
             "%s[%s] %s(%d): %s%s\n",
             color, timestamp, name, pid, message, ANSI_RESET);
    
    write(STDOUT_FILENO, console_line, strlen(console_line));
    
    // Linia do pliku (bez kolorów)
    snprintf(file_line, sizeof(file_line),
             "[%s] %s(%d): %s\n",
             timestamp, name, pid, message);
    
    pthread_mutex_lock(&file_mutex);
    if (file_log != NULL) {
        fprintf(file_log, "%s", file_line);
        fflush(file_log);  
    }
    pthread_mutex_unlock(&file_mutex);
}

void logger_report(const char* format, ...) {
    char line[1024];
    char file_line[1100];
    char timestamp[32];
    
    get_timestamp(timestamp, sizeof(timestamp));
    
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    
    // Zapisanie do pliku raportu
    pthread_mutex_lock(&file_mutex);
    if (file_report != NULL) {
        fprintf(file_report, "%s\n", line);
        fflush(file_report);
    }
    
    // Zapisanie również do pliku logów
    snprintf(file_line, sizeof(file_line),
             "[%s] RAPORT: %s\n",
             timestamp, line);
    if (file_log != NULL) {
        fprintf(file_log, "%s", file_line);
        fflush(file_log);
    }
    pthread_mutex_unlock(&file_mutex);
    
    char console_line[1100];
    snprintf(console_line, sizeof(console_line), "%s[%s] RAPORT: %s%s\n", 
             ANSI_BRIGHT_GREEN, timestamp, line, ANSI_RESET);
    write(STDOUT_FILENO, console_line, strlen(console_line));
}

// Zapis tylko do pliku raportu 
void logger_report_file_only(const char* format, ...) {
    char line[1024];
    
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    
    pthread_mutex_lock(&file_mutex);
    if (file_report != NULL) {
        fprintf(file_report, "%s\n", line);
        fflush(file_report);
    }
    pthread_mutex_unlock(&file_mutex);
}

void logger_clear_files(void) {
    // czyszczenie plików przed nową symulacją
    FILE* fd = fopen(LOG_FILE, "w");
    if (fd != NULL) fclose(fd);
    
    fd = fopen(REPORT_FILE, "w");
    if (fd != NULL) fclose(fd);
}

// Rejestrowanie przejścia przez bramkę
void rejestruj_przejscie_bramki(int ticket_id, int gate_number, int gate_type) {
    if (ticket_id <= 0 || gate_number <= 0 || gate_type <= 0) {
        return;
    }
    int shm_id = polacz_pamiec();
    SharedMemory* shm = dolacz_pamiec(shm_id);
    int sem_id = polacz_semafory();
    
    sem_opusc(sem_id, SEM_MAIN);
    
    int idx = shm->gate_passages_count;
    if (idx < MAX_GATE_PASSAGES) {
        shm->gate_passages[idx].ticket_id = ticket_id;
        shm->gate_passages[idx].timestamp = time(NULL);
        shm->gate_passages[idx].gate_number = gate_type * 10 + gate_number; // np. 14 = bramka wejściowa #4, 23 = bramka peronowa #3
        shm->gate_passages_count++;
    }
    
    sem_podnies(sem_id, SEM_MAIN);
    odlacz_pamiec(shm);
}

// Rejestrowanie zjazdu dla danego biletu
void rejestruj_zjazd(int ticket_id) {
    int shm_id = polacz_pamiec();
    SharedMemory* shm = dolacz_pamiec(shm_id);
    int sem_id = polacz_semafory();
    
    sem_opusc(sem_id, SEM_MAIN);
    
    if (ticket_id > 0 && ticket_id < MAX_TICKETS) {
        shm->ticket_rides[ticket_id]++;
    }
    
    sem_podnies(sem_id, SEM_MAIN);
    odlacz_pamiec(shm);
}

void generuj_raport_koncowy(void) {
    // Połącz z pamięcią dzieloną
    int shm_id = polacz_pamiec();
    SharedMemory* shm = dolacz_pamiec(shm_id);
    int sem_id = polacz_semafory();
    
    sem_opusc(sem_id, SEM_MAIN);
    
    // Oblicz statystyki
    int total_tickets = 0;
    for (int i = 0; i < TICKET_TYPE_COUNT; i++) {
        total_tickets += shm->tickets_sold[i];
    }
    
    double avg_per_chair = shm->chair_departures > 0 ?
        (double)shm->passengers_transported / shm->chair_departures : 0.0;
    
    int total_trail = shm->trail_usage[TRAIL_T1] + shm->trail_usage[TRAIL_T2] + shm->trail_usage[TRAIL_T3];
    
    // Kopiuj dane do lokalnych zmiennych (żeby szybciej zwolnić semafor)
    int passages_count = shm->gate_passages_count;
    GatePassage* passages = NULL;
    if (passages_count > 0) {
        passages = malloc(passages_count * sizeof(GatePassage));
        if (passages) {
            memcpy(passages, shm->gate_passages, passages_count * sizeof(GatePassage));
        }
    }
    
    // Kopiuj dane o zjazdach per bilet
    int max_ticket_id = shm->next_ticket_id;
    int* ticket_rides = NULL;
    if (max_ticket_id > 0) {
        ticket_rides = malloc(max_ticket_id * sizeof(int));
        if (ticket_rides) {
            memcpy(ticket_rides, shm->ticket_rides, max_ticket_id * sizeof(int));
        }
    }
    
    // Zapisz resztę statystyk
    int tickets_sold[TICKET_TYPE_COUNT];
    memcpy(tickets_sold, shm->tickets_sold, sizeof(tickets_sold));
    int chair_departures = shm->chair_departures;
    int passengers_transported = shm->passengers_transported;
    int cyclists_transported = shm->cyclists_transported;
    int pedestrians_transported = shm->pedestrians_transported;
    int vip_served = shm->vip_served;
    int children_with_guardian = shm->children_with_guardian;
    int rejected_expired = shm->rejected_expired;
    int trail_usage[TRAIL_COUNT];
    memcpy(trail_usage, shm->trail_usage, sizeof(trail_usage));
    int total_tourists_created = shm->total_tourists_created;
    int total_tourists_finished = shm->total_tourists_finished;
    
    sem_podnies(sem_id, SEM_MAIN);
    
    // Generowanie raportu
    logger_report("");
    logger_report("============================================================");
    logger_report("         RAPORT KONCOWY SYMULACJI KOLEI LINOWEJ");
    logger_report("============================================================");
    logger_report("");
    logger_report("1. SPRZEDAZ BILETOW:");
    logger_report("   Jednorazowe (SINGLE):     %d szt.", tickets_sold[TICKET_SINGLE]);
    logger_report("   Czasowe TK1 (1h):         %d szt.", tickets_sold[TICKET_TK1]);
    logger_report("   Czasowe TK2 (2h):         %d szt.", tickets_sold[TICKET_TK2]);
    logger_report("   Czasowe TK3 (3h):         %d szt.", tickets_sold[TICKET_TK3]);
    logger_report("   Dzienne (DAILY):          %d szt.", tickets_sold[TICKET_DAILY]);
    logger_report("   RAZEM:                    %d szt.", total_tickets);
    logger_report("");
    logger_report("2. STATYSTYKI PRZEJAZDOW:");
    logger_report("   Odjazdy krzeselek:        %d", chair_departures);
    logger_report("   Przewiezione osoby:       %d", passengers_transported);
    logger_report("   - Rowerzysci:             %d", cyclists_transported);
    logger_report("   - Piesi:                  %d", pedestrians_transported);
    logger_report("   Sr. osob/krzeslo:         %.2f", avg_per_chair);
    logger_report("");
    logger_report("3. KATEGORIE SPECJALNE:");
    logger_report("   VIP obsluzeni:            %d", vip_served);
    logger_report("   Dzieci z opiekunem:       %d", children_with_guardian);
    logger_report("   Odrzuceni (wygasly):      %d", rejected_expired);
    logger_report("");
    logger_report("4. TRASY ZJAZDOWE:");
    logger_report("   T1 (latwa, %ds):          %d turystow", TRAIL_T1_TIME, trail_usage[TRAIL_T1]);
    logger_report("   T2 (srednia, %ds):        %d turystow", TRAIL_T2_TIME, trail_usage[TRAIL_T2]);
    logger_report("   T3 (trudna, %ds):         %d turystow", TRAIL_T3_TIME, trail_usage[TRAIL_T3]);
    logger_report("   RAZEM na trasach:         %d turystow", total_trail);
    logger_report("");
    logger_report("5. PODSUMOWANIE TURYSTOW:");
    logger_report("   Utworzonych turystow:     %d", total_tourists_created);
    logger_report("   Zakonczonych wizyt:       %d", total_tourists_finished);
    logger_report("");
    
    // Rejestracja przejść bramkowych
    logger_report("6. REJESTRACJA PRZEJSC BRAMKOWYCH:");
    
    int valid_passages = 0;
    if (passages) {
        for (int i = 0; i < passages_count; i++) {
            if (passages[i].ticket_id > 0 && passages[i].timestamp > 0) {
                valid_passages++;
            }
        }
    }
    logger_report("   Zarejestrowanych przejsc: %d", valid_passages);
    logger_report("");
    
    if (passages && valid_passages > 0) {
        
        // Szczegółowe przejścia do pliku
        logger_report_file_only("   --- SZCZEGOLOWA HISTORIA PRZEJSC ---");
        
        for (int i = 0; i < passages_count; i++) {
            if (passages[i].ticket_id <= 0 || passages[i].timestamp <= 0) {
                continue;
            }
            
            struct tm* tm_info = localtime(&passages[i].timestamp);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
            
            int gate_type = passages[i].gate_number / 10;
            int gate_num = passages[i].gate_number % 10;
            const char* gate_type_str = (gate_type == 1) ? "wejsciowa" : "peronowa";
            
            // Tylko do pliku - nie do konsoli
            logger_report_file_only("   [%s] Bilet #%d - bramka %s #%d", 
                         time_str, passages[i].ticket_id, gate_type_str, gate_num);
        }
        logger_report_file_only("");
        logger_report("");
    }
    
    // Sekcja 7: Podsumowanie zjazdów per bilet
    logger_report("7. ZJAZDY PER BILET/KARNET:");
    if (ticket_rides && max_ticket_id > 0) {
        // Policz bilety z różną liczbą zjazdów
        int tickets_0_rides = 0;
        int tickets_1_ride = 0;
        int tickets_2_rides = 0;
        int tickets_3plus_rides = 0;
        int max_rides = 0;
        int max_rides_ticket = 0;
        
        for (int i = 1; i < max_ticket_id; i++) {
            if (ticket_rides[i] == 0) tickets_0_rides++;
            else if (ticket_rides[i] == 1) tickets_1_ride++;
            else if (ticket_rides[i] == 2) tickets_2_rides++;
            else tickets_3plus_rides++;
            
            if (ticket_rides[i] > max_rides) {
                max_rides = ticket_rides[i];
                max_rides_ticket = i;
            }
        }
        
        logger_report("   Bilety z 0 zjazdami:      %d", tickets_0_rides);
        logger_report("   Bilety z 1 zjazdem:       %d", tickets_1_ride);
        logger_report("   Bilety z 2 zjazdami:      %d", tickets_2_rides);
        logger_report("   Bilety z 3+ zjazdami:     %d", tickets_3plus_rides);
        if (max_rides > 0) {
            logger_report("   Najwiecej zjazdow:        Bilet #%d (%d zjazdow)", max_rides_ticket, max_rides);
        }
        logger_report("");
        
        // Wypisz szczegóły dla biletów z więcej niż 1 zjazdem (max 30)
        logger_report("   --- BILETY Z WIELOMA ZJAZDAMI (max 30) ---");
        int printed = 0;
        for (int i = 1; i < max_ticket_id && printed < 30; i++) {
            if (ticket_rides[i] > 1) {
                logger_report("   Bilet #%d: %d zjazdow", i, ticket_rides[i]);
                printed++;
            }
        }
    }
    logger_report("");
    logger_report("============================================================");
    logger_report("");
    
    // Zwolnij pamięć
    if (passages) free(passages);
    if (ticket_rides) free(ticket_rides);
    
    odlacz_pamiec(shm);
}
