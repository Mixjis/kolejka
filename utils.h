#ifndef UTILS_H
#define UTILS_H

#include "struktury.h"
#include <sys/types.h>
#include <stdbool.h>

// funkcje semaforów
int utworz_semafory(void);
int polacz_semafory(void);
void usun_semafory(int sem_id);

void sem_podnies(int sem_id, int sem_num);
void sem_opusc(int sem_id, int sem_num);
void sem_podnies_bez_undo(int sem_id, int sem_num);
void sem_opusc_bez_undo(int sem_id, int sem_num);
int sem_probuj_opusc(int sem_id, int sem_num);
int sem_probuj_opusc_bez_undo(int sem_id, int sem_num);
int sem_opusc_timeout(int sem_id, int sem_num, int timeout_ms);
void sem_czekaj_na_zero(int sem_id, int sem_num);
int sem_pobierz_wartosc(int sem_id, int sem_num);
void sem_ustaw_wartosc(int sem_id, int sem_num, int value);

// funkcje pamięci dzielonej
int utworz_pamiec(void);
int polacz_pamiec(void);
void usun_pamiec(int shm_id);
SharedMemory* dolacz_pamiec(int shm_id);
void odlacz_pamiec(SharedMemory* shm);

// funkcje kolejek komunikatów
int utworz_kolejke(void);
int utworz_kolejke_worker(void);
int polacz_kolejke(void);
int polacz_kolejke_worker(void);
void usun_kolejke(int msg_id);

bool wyslij_komunikat(int msg_id, Message* msg);
bool wyslij_komunikat_nowait(int msg_id, Message* msg);
bool odbierz_komunikat(int msg_id, Message* msg, long mtype, bool blocking);
bool odbierz_komunikat_timeout(int msg_id, Message* msg, long mtype, int timeout_ms);

// funkcje pomocnicze
key_t utworz_klucz(int id);
void czysc_zasoby(void);
const char* nazwa_biletu(TicketType type);
const char* nazwa_trasy(TrailType trail);
int cena_biletu(TicketType type, bool discount);
int czas_waznosci(TicketType type);

#endif // UTILS_H
