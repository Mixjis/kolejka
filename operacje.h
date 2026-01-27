// operacje.h - deklaracje funkcji IPC i pomocniczych

#ifndef OPERACJE_H
#define OPERACJE_H

#include "struktury.h"
#include <sys/types.h>
#include <stdbool.h>

// ==== FUNKCJE SEMAFORÓW ====
int utworz_semafory(void);
int polacz_semafory(void);
void usun_semafory(int sem_id);

void sem_podnies(int sem_id, int sem_num);
void sem_opusc(int sem_id, int sem_num);
void sem_podnies_bez_undo(int sem_id, int sem_num);
void sem_opusc_bez_undo(int sem_id, int sem_num);
int sem_probuj_opusc(int sem_id, int sem_num);
void sem_czekaj_na_zero(int sem_id, int sem_num);
int sem_pobierz_wartosc(int sem_id, int sem_num);
void sem_ustaw_wartosc(int sem_id, int sem_num, int value);

// ==== FUNKCJE PAMIĘCI DZIELONEJ ====
int utworz_pamiec(void);
int polacz_pamiec(void);
void usun_pamiec(int shm_id);
SharedMemory* dolacz_pamiec(int shm_id);
void odlacz_pamiec(SharedMemory* shm);

// ==== FUNKCJE KOLEJEK KOMUNIKATÓW ====
int utworz_kolejke(void);
int utworz_kolejke_worker(void);
int polacz_kolejke(void);
int polacz_kolejke_worker(void);
void usun_kolejke(int msg_id);

bool wyslij_komunikat(int msg_id, Message* msg);
bool wyslij_komunikat_nowait(int msg_id, Message* msg);
bool odbierz_komunikat(int msg_id, Message* msg, long mtype, bool blocking);
bool odbierz_komunikat_timeout(int msg_id, Message* msg, long mtype, int timeout_ms);

// ==== FUNKCJE POMOCNICZE ====
key_t utworz_klucz(int id);
void czysc_zasoby(void);
const char* nazwa_biletu(TicketType type);
const char* nazwa_trasy(TrailType trail);
int cena_biletu(TicketType type, bool discount);
int czas_waznosci(TicketType type);

// ==== OBSŁUGA BŁĘDÓW ====
void blad_krytyczny(const char* msg);
void blad_ostrzezenie(const char* msg);

#endif // OPERACJE_H
