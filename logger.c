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

static int fd_log = -1;
static int fd_report = -1;
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

// Bezpieczny zapis do pliku
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
    
    if (fd_log < 0) {
        fd_log = open(LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        if (fd_log < 0) {
            perror("Nie można otworzyć pliku logów");
        }
    }
    
    if (fd_report < 0) {
        fd_report = open(REPORT_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        if (fd_report < 0) {
            perror("Nie można otworzyć pliku raportu");
        }
    }
    
    pthread_mutex_unlock(&file_mutex);
    
    // Nagłówek pliku logów
    if (fd_log >= 0) {
        const char* header = "========== START SYMULACJI KOLEI LINOWEJ ==========\n";
        safe_write(fd_log, header, strlen(header));
    }
}

void logger_close(void) {
    pthread_mutex_lock(&file_mutex);
    
    if (fd_log >= 0) {
        const char* footer = "========== KONIEC SYMULACJI ==========\n";
        safe_write(fd_log, footer, strlen(footer));
        close(fd_log);
        fd_log = -1;
    }
    
    if (fd_report >= 0) {
        close(fd_report);
        fd_report = -1;
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
    
    // Linia do konsoli
    snprintf(console_line, sizeof(console_line),
             "%s[%s] %s(%d): %s%s\n",
             color, timestamp, name, pid, message, ANSI_RESET);
    
    write(STDOUT_FILENO, console_line, strlen(console_line));
    
    // Linia do pliku (sekwencyjna - z mutexem)
    pthread_mutex_lock(&file_mutex);
    log_sequence++;
    snprintf(file_line, sizeof(file_line),
             "[%06d][%s] %s(%d): %s\n",
             log_sequence, timestamp, name, pid, message);
    
    if (fd_log >= 0) {
        safe_write(fd_log, file_line, strlen(file_line));
        fsync(fd_log);
    }
    pthread_mutex_unlock(&file_mutex);
}

void logger_report(const char* format, ...) {
    char line[1024];
    
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    
    // Zapisanie do pliku raportu
    pthread_mutex_lock(&file_mutex);
    if (fd_report >= 0) {
        safe_write(fd_report, line, strlen(line));
        safe_write(fd_report, "\n", 1);
    }
    pthread_mutex_unlock(&file_mutex);
    
    char console_line[1100];
    snprintf(console_line, sizeof(console_line), "%s%s%s\n", 
             ANSI_BRIGHT_GREEN, line, ANSI_RESET);
    write(STDOUT_FILENO, console_line, strlen(console_line));
}

void logger_clear_files(void) {
    // czyszczenie plików przed nową symulacją
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    
    fd = open(REPORT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
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
    
    sem_podnies(sem_id, SEM_MAIN);
    
    // Generuj raport
    logger_report("");
    logger_report("============================================================");
    logger_report("         RAPORT KONCOWY SYMULACJI KOLEI LINOWEJ");
    logger_report("============================================================");
    logger_report("");
    logger_report("1. SPRZEDAZ BILETOW:");
    logger_report("   Jednorazowe (SINGLE):     %d szt.", shm->tickets_sold[TICKET_SINGLE]);
    logger_report("   Czasowe TK1 (1h):         %d szt.", shm->tickets_sold[TICKET_TK1]);
    logger_report("   Czasowe TK2 (2h):         %d szt.", shm->tickets_sold[TICKET_TK2]);
    logger_report("   Czasowe TK3 (3h):         %d szt.", shm->tickets_sold[TICKET_TK3]);
    logger_report("   Dzienne (DAILY):          %d szt.", shm->tickets_sold[TICKET_DAILY]);
    logger_report("   RAZEM:                    %d szt.", total_tickets);
    logger_report("");
    logger_report("2. STATYSTYKI PRZEJAZDOW:");
    logger_report("   Odjazdy krzeselek:        %d", shm->chair_departures);
    logger_report("   Przewiezione osoby:       %d", shm->passengers_transported);
    logger_report("   - Rowerzysci:             %d", shm->cyclists_transported);
    logger_report("   - Piesi:                  %d", shm->pedestrians_transported);
    logger_report("   Sr. osob/krzeslo:         %.2f", avg_per_chair);
    logger_report("");
    logger_report("3. KATEGORIE SPECJALNE:");
    logger_report("   VIP obsluzeni:            %d", shm->vip_served);
    logger_report("   Dzieci z opiekunem:       %d", shm->children_with_guardian);
    logger_report("   Odrzuceni (wygasly):      %d", shm->rejected_expired);
    logger_report("");
    logger_report("4. TRASY ZJAZDOWE:");
    logger_report("   T1 (latwa, %ds):          %d turystow", TRAIL_T1_TIME, shm->trail_usage[TRAIL_T1]);
    logger_report("   T2 (srednia, %ds):        %d turystow", TRAIL_T2_TIME, shm->trail_usage[TRAIL_T2]);
    logger_report("   T3 (trudna, %ds):         %d turystow", TRAIL_T3_TIME, shm->trail_usage[TRAIL_T3]);
    logger_report("   RAZEM na trasach:         %d turystow", total_trail);
    logger_report("");
    logger_report("5. REJESTRACJA PRZEJSC BRAMKOWYCH:");
    logger_report("   Zarejestrowanych przejsc: %d", shm->gate_passages_count);
    logger_report("");
    logger_report("6. PODSUMOWANIE TURYSTOW:");
    logger_report("   Utworzonych turystow:     %d", shm->total_tourists_created);
    logger_report("   Zakonczonych wizyt:       %d", shm->total_tourists_finished);
    logger_report("");
    logger_report("============================================================");
    logger_report("");
    
    odlacz_pamiec(shm);
}
