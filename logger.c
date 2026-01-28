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
#include <sys/sem.h>
#include "logger.h"
#include "struktury.h"
#include "operacje.h"

#define LOG_FILE "kolej_log.txt"
#define REPORT_FILE "raport_karnetow.txt"

static int log_fd = -1;        // Deskryptor pliku logów (używamy write() zamiast fprintf dla atomowości)
static int report_fd = -1;     // Deskryptor pliku raportu

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
    // Otwórz plik logów w trybie zapisu (nadpisz) z O_APPEND dla atomowości
    log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) {
        perror("Nie można otworzyć pliku logów");
    }
    
    // Otwórz plik raportu
    report_fd = open(REPORT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (report_fd < 0) {
        perror("Nie można otworzyć pliku raportu");
    }
    
    // Nagłówek pliku logów
    if (log_fd >= 0) {
        const char* header = "========== START SYMULACJI KOLEI LINOWEJ ==========\n";
        safe_write(log_fd, header, strlen(header));
    }
}

void logger_init_child(void) {
    // Zamknij stare deskryptory jeśli istnieją
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
    if (report_fd >= 0) {
        close(report_fd);
        report_fd = -1;
    }
    
    // Otwórz ponownie w trybie append - O_APPEND zapewnia atomowe dopisywanie
    log_fd = open(LOG_FILE, O_WRONLY | O_APPEND);
    if (log_fd < 0) {
        perror("Nie można otworzyć pliku logów (child)");
    }
    
    report_fd = open(REPORT_FILE, O_WRONLY | O_APPEND);
    if (report_fd < 0) {
        perror("Nie można otworzyć pliku raportu (child)");
    }
}

void logger_close(void) {
    if (log_fd >= 0) {
        const char* footer = "========== KONIEC SYMULACJI ==========\n";
        safe_write(log_fd, footer, strlen(footer));
        close(log_fd);
        log_fd = -1;
    }
    
    if (report_fd >= 0) {
        close(report_fd);
        report_fd = -1;
    }
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
    
    // Linia do pliku (bez kolorów) - O_APPEND zapewnia atomowość dla linii < PIPE_BUF
    int len = snprintf(file_line, sizeof(file_line),
             "[%s] %s(%d): %s\n",
             timestamp, name, pid, message);
    
    if (log_fd >= 0 && len > 0) {
        safe_write(log_fd, file_line, len);
    }
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
    int report_len = snprintf(file_line, sizeof(file_line), "%s\n", line);
    if (report_fd >= 0 && report_len > 0) {
        safe_write(report_fd, file_line, report_len);
    }
    
    // Zapisanie również do pliku logów
    int log_len = snprintf(file_line, sizeof(file_line),
             "[%s] RAPORT: %s\n",
             timestamp, line);
    if (log_fd >= 0 && log_len > 0) {
        safe_write(log_fd, file_line, log_len);
    }
    
    char console_line[1100];
    snprintf(console_line, sizeof(console_line), "%s[%s] RAPORT: %s%s\n", 
             ANSI_BRIGHT_GREEN, timestamp, line, ANSI_RESET);
    write(STDOUT_FILENO, console_line, strlen(console_line));
}

// Zapis tylko do pliku raportu 
void logger_report_file_only(const char* format, ...) {
    char line[1024];
    char file_line[1100];
    
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    
    int len = snprintf(file_line, sizeof(file_line), "%s\n", line);
    if (report_fd >= 0 && len > 0) {
        safe_write(report_fd, file_line, len);
    }
}

void logger_clear_files(void) {
    // czyszczenie plików przed nową symulacją
    FILE* fd = fopen(LOG_FILE, "w");
    if (fd != NULL) fclose(fd);
    
    fd = fopen(REPORT_FILE, "w");
    if (fd != NULL) fclose(fd);
}

// Rejestrowanie zjazdu dla danego biletu
// UWAGA: Wołający musi SAM trzymać SEM_MAIN i przekazać shm
void rejestruj_zjazd(int ticket_id) {
    // Ta funkcja jest teraz pusta - logika przeniesiona do tourist.c
    (void)ticket_id;
}

// Rejestrowanie przejścia przez bramkę (id karnetu - godzina)
void rejestruj_przejscie_bramki(int ticket_id) {
    int shm_id = polacz_pamiec();
    SharedMemory* shm = dolacz_pamiec(shm_id);
    int sem_id = polacz_semafory();
    
    sem_opusc(sem_id, SEM_MAIN);
    if (shm->gate_entries_count < MAX_GATE_ENTRIES) {
        shm->gate_entries[shm->gate_entries_count].ticket_id = ticket_id;
        shm->gate_entries[shm->gate_entries_count].entry_time = time(NULL);
        shm->gate_entries_count++;
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
    
    // Kopiuj dane o zjazdach per bilet
    int max_ticket_id = shm->next_ticket_id;
    int* ticket_rides = NULL;
    if (max_ticket_id > 0) {
        ticket_rides = malloc(max_ticket_id * sizeof(int));
        if (ticket_rides) {
            memcpy(ticket_rides, shm->ticket_rides, max_ticket_id * sizeof(int));
        }
    }
    
    // Kopiuj dane o przejściach przez bramki
    int gate_entries_count = shm->gate_entries_count;
    struct {
        int ticket_id;
        time_t entry_time;
    } *gate_entries = NULL;
    
    if (gate_entries_count > 0) {
        gate_entries = malloc(gate_entries_count * sizeof(*gate_entries));
        if (gate_entries) {
            for (int i = 0; i < gate_entries_count; i++) {
                gate_entries[i].ticket_id = shm->gate_entries[i].ticket_id;
                gate_entries[i].entry_time = shm->gate_entries[i].entry_time;
            }
        }
    }
    
    sem_podnies(sem_id, SEM_MAIN);
    
    // Generowanie prostego raportu - tylko do pliku, bez konsoli
    logger_report_file_only("============================================================");
    logger_report_file_only("         RAPORT KARNETOW - KOLEJ LINOWA");
    logger_report_file_only("============================================================");
    logger_report_file_only("");
    
    // Sekcja 1: Lista przejść przez bramki (id karnetu - godzina)
    logger_report_file_only("PRZEJSCIA PRZEZ BRAMKI (ID KARNETU - GODZINA):");
    logger_report_file_only("------------------------------------------------------------");
    
    if (gate_entries && gate_entries_count > 0) {
        for (int i = 0; i < gate_entries_count; i++) {
            struct tm* tm_info = localtime(&gate_entries[i].entry_time);
            char time_str[32];
            snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
                     tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
            logger_report_file_only("Karnet #%d - %s", gate_entries[i].ticket_id, time_str);
        }
    } else {
        logger_report_file_only("Brak zarejestrowanych przejsc.");
    }
    
    logger_report_file_only("");
    logger_report_file_only("============================================================");
    logger_report_file_only("");
    
    // Sekcja 2: Podsumowanie liczby przejazdów per karnet
    logger_report_file_only("PODSUMOWANIE LICZBY PRZEJAZDOW PER KARNET:");
    logger_report_file_only("------------------------------------------------------------");
    
    if (ticket_rides && max_ticket_id > 0) {
        for (int i = 1; i < max_ticket_id; i++) {
            if (ticket_rides[i] > 0) {
                logger_report_file_only("Karnet #%d: %d przejazd(ow)", i, ticket_rides[i]);
            }
        }
    } else {
        logger_report_file_only("Brak danych o przejazdach.");
    }
    
    logger_report_file_only("");
    logger_report_file_only("============================================================");
    logger_report_file_only("");
    
    // Zwolnij pamięć
    if (ticket_rides) free(ticket_rides);
    if (gate_entries) free(gate_entries);
    
    odlacz_pamiec(shm);
}