// logger.h - deklaracje funkcji logowania

#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>

// Typy nadawców logów (dla kolorowania)
typedef enum {
    LOG_SYSTEM = 0,
    LOG_CASHIER,
    LOG_WORKER1,
    LOG_WORKER2,
    LOG_TOURIST,
    LOG_VIP,
    LOG_CHAIR,
    LOG_EMERGENCY,
    LOG_REPORT
} LogSender;

// Inicjalizacja i zamykanie loggera
void logger_init(void);

// inicjalizacja dla procesów potomnych
void logger_init_child(void);

// Zamknięcie loggera
void logger_close(void);

// Główna funkcja logowania
void logger(LogSender sender, const char* format, ...);

// Logowanie bez koloru (do pliku raportu)
void logger_report(const char* format, ...);

// Zapis tylko do pliku raportu
void logger_report_file_only(const char* format, ...);

// Czyszczenie plików logów
void logger_clear_files(void);

// Rejestrowanie zjazdu (zwiększa licznik zjazdów dla danego biletu)
void rejestruj_zjazd(int ticket_id);

// Rejestrowanie przejścia przez bramkę (id karnetu - godzina)
void rejestruj_przejscie_bramki(int ticket_id, int gate_number);

// Generowanie raportu końcowego
void generuj_raport_koncowy(void);

#endif // LOGGER_H
