#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include "operacje.h"
#include "struktury.h"

// Union dla semctl
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

// ==== KLUCZE ====
key_t utworz_klucz(int id) {
    key_t klucz = ftok(IPC_KEY_PATH, id);
    if (klucz == -1) {
        perror("Błąd ftok");
        exit(1);
    }
    return klucz;
}

// ==== SEMAFORY ====
int utworz_semafory(void) {
    key_t klucz = utworz_klucz(IPC_KEY_SEM);
    int sem_id = semget(klucz, SEM_COUNT, IPC_CREAT | IPC_EXCL | 0600);
    
    if (sem_id == -1) {
        if (errno == EEXIST) {
            // Usuń stare i utwórz nowe
            sem_id = semget(klucz, SEM_COUNT, 0600);
            if (sem_id != -1) {
                semctl(sem_id, 0, IPC_RMID);
            }
            sem_id = semget(klucz, SEM_COUNT, IPC_CREAT | IPC_EXCL | 0600);
        }
        if (sem_id == -1) {
            perror("Błąd semget (tworzenie)");
            exit(1);
        }
    }
    
    union semun arg;
    
    // SEM_MAIN - mutex (1)
    arg.val = 1;
    if (semctl(sem_id, SEM_MAIN, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_MAIN");
        exit(1);
    }
    
    // SEM_STATION - limit osób na stacji (N)
    arg.val = STATION_CAPACITY;
    if (semctl(sem_id, SEM_STATION, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_STATION");
        exit(1);
    }
    
    // SEM_PLATFORM - mutex peronu
    arg.val = 1;
    if (semctl(sem_id, SEM_PLATFORM, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_PLATFORM");
        exit(1);
    }
    
    // SEM_CHAIRS - dostępne krzesełka
    arg.val = MAX_ACTIVE_CHAIRS;
    if (semctl(sem_id, SEM_CHAIRS, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_CHAIRS");
        exit(1);
    }
    
    // SEM_GATE_ENTRY - bramki wejściowe
    arg.val = ENTRY_GATES;
    if (semctl(sem_id, SEM_GATE_ENTRY, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_GATE_ENTRY");
        exit(1);
    }
    
    // SEM_GATE_PLATFORM - bramki na peron
    arg.val = PLATFORM_GATES;
    if (semctl(sem_id, SEM_GATE_PLATFORM, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_GATE_PLATFORM");
        exit(1);
    }
    
    // SEM_GATE_EXIT - wyjścia górna stacja
    arg.val = EXIT_GATES;
    if (semctl(sem_id, SEM_GATE_EXIT, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_GATE_EXIT");
        exit(1);
    }
    
    // SEM_EMERGENCY - flaga awarii (1 = normalnie, 0 = stop)
    arg.val = 1;
    if (semctl(sem_id, SEM_EMERGENCY, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_EMERGENCY");
        exit(1);
    }
    
    // SEM_WORKER_SYNC - synchronizacja pracowników
    arg.val = 0;
    if (semctl(sem_id, SEM_WORKER_SYNC, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_WORKER_SYNC");
        exit(1);
    }
    
    // SEM_LOG_FILE - mutex pliku logów
    arg.val = 1;
    if (semctl(sem_id, SEM_LOG_FILE, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_LOG_FILE");
        exit(1);
    }
    
    // SEM_REPORT - mutex raportu
    arg.val = 1;
    if (semctl(sem_id, SEM_REPORT, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL SEM_REPORT");
        exit(1);
    }
    
    return sem_id;
}

int polacz_semafory(void) {
    key_t klucz = utworz_klucz(IPC_KEY_SEM);
    int sem_id = semget(klucz, SEM_COUNT, 0600);
    if (sem_id == -1) {
        perror("Błąd semget (połączenie)");
        exit(1);
    }
    return sem_id;
}

void usun_semafory(int sem_id) {
    if (semctl(sem_id, 0, IPC_RMID) == -1) {
        perror("Błąd semctl IPC_RMID");
    }
}

void sem_podnies(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = 1;
    op.sem_flg = SEM_UNDO;
    
    while (semop(sem_id, &op, 1) == -1) {
        if (errno == EINTR) continue;
        perror("Błąd semop (podniesienie)");
        break;
    }
}

void sem_opusc(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = -1;
    op.sem_flg = SEM_UNDO;
    
    while (semop(sem_id, &op, 1) == -1) {
        if (errno == EINTR) continue;
        perror("Błąd semop (opuszczenie)");
        break;
    }
}

void sem_podnies_bez_undo(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = 1;
    op.sem_flg = 0;
    
    while (semop(sem_id, &op, 1) == -1) {
        if (errno == EINTR) continue;
        perror("Błąd semop (podniesienie bez undo)");
        break;
    }
}

void sem_opusc_bez_undo(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = -1;
    op.sem_flg = 0;
    
    while (semop(sem_id, &op, 1) == -1) {
        if (errno == EINTR) continue;
        perror("Błąd semop (opuszczenie bez undo)");
        break;
    }
}

int sem_probuj_opusc(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = -1;
    op.sem_flg = IPC_NOWAIT | SEM_UNDO;
    
    if (semop(sem_id, &op, 1) == -1) {
        if (errno == EAGAIN || errno == EINTR) {
            return 0; // Nie udało się
        }
        perror("Błąd semop (próba opuszczenia)");
        return -1;
    }
    return 1; // Udało się
}

void sem_czekaj_na_zero(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = 0;
    op.sem_flg = 0;
    
    while (semop(sem_id, &op, 1) == -1) {
        if (errno == EINTR) continue;
        perror("Błąd semop (czekanie na zero)");
        break;
    }
}

int sem_pobierz_wartosc(int sem_id, int sem_num) {
    int val = semctl(sem_id, sem_num, GETVAL);
    if (val == -1) {
        perror("Błąd semctl GETVAL");
    }
    return val;
}

void sem_ustaw_wartosc(int sem_id, int sem_num, int value) {
    union semun arg;
    arg.val = value;
    if (semctl(sem_id, sem_num, SETVAL, arg) == -1) {
        perror("Błąd semctl SETVAL");
    }
}

// ==== PAMIĘĆ DZIELONA ====
int utworz_pamiec(void) {
    key_t klucz = utworz_klucz(IPC_KEY_SHM);
    int shm_id = shmget(klucz, sizeof(SharedMemory), IPC_CREAT | IPC_EXCL | 0600);
    
    if (shm_id == -1) {
        if (errno == EEXIST) {
            shm_id = shmget(klucz, sizeof(SharedMemory), 0600);
            if (shm_id != -1) {
                shmctl(shm_id, IPC_RMID, NULL);
            }
            shm_id = shmget(klucz, sizeof(SharedMemory), IPC_CREAT | IPC_EXCL | 0600);
        }
        if (shm_id == -1) {
            perror("Błąd shmget (tworzenie)");
            exit(1);
        }
    }
    
    return shm_id;
}

int polacz_pamiec(void) {
    key_t klucz = utworz_klucz(IPC_KEY_SHM);
    int shm_id = shmget(klucz, sizeof(SharedMemory), 0600);
    if (shm_id == -1) {
        perror("Błąd shmget (połączenie)");
        exit(1);
    }
    return shm_id;
}

void usun_pamiec(int shm_id) {
    if (shmctl(shm_id, IPC_RMID, NULL) == -1) {
        perror("Błąd shmctl IPC_RMID");
    }
}

SharedMemory* dolacz_pamiec(int shm_id) {
    SharedMemory* shm = (SharedMemory*)shmat(shm_id, NULL, 0);
    if (shm == (SharedMemory*)-1) {
        perror("Błąd shmat");
        exit(1);
    }
    return shm;
}

void odlacz_pamiec(SharedMemory* shm) {
    if (shmdt(shm) == -1) {
        perror("Błąd shmdt");
    }
}

// ==== KOLEJKI KOMUNIKATÓW ====
int utworz_kolejke(void) {
    key_t klucz = utworz_klucz(IPC_KEY_MSG);
    int msg_id = msgget(klucz, IPC_CREAT | IPC_EXCL | 0600);
    
    if (msg_id == -1) {
        if (errno == EEXIST) {
            msg_id = msgget(klucz, 0600);
            if (msg_id != -1) {
                msgctl(msg_id, IPC_RMID, NULL);
            }
            msg_id = msgget(klucz, IPC_CREAT | IPC_EXCL | 0600);
        }
        if (msg_id == -1) {
            perror("Błąd msgget (tworzenie)");
            exit(1);
        }
    }
    
    return msg_id;
}

int utworz_kolejke_worker(void) {
    key_t klucz = utworz_klucz(IPC_KEY_MSG_WORKER);
    int msg_id = msgget(klucz, IPC_CREAT | IPC_EXCL | 0600);
    
    if (msg_id == -1) {
        if (errno == EEXIST) {
            msg_id = msgget(klucz, 0600);
            if (msg_id != -1) {
                msgctl(msg_id, IPC_RMID, NULL);
            }
            msg_id = msgget(klucz, IPC_CREAT | IPC_EXCL | 0600);
        }
        if (msg_id == -1) {
            perror("Błąd msgget worker (tworzenie)");
            exit(1);
        }
    }
    
    return msg_id;
}

int polacz_kolejke(void) {
    key_t klucz = utworz_klucz(IPC_KEY_MSG);
    int msg_id = msgget(klucz, 0600);
    if (msg_id == -1) {
        perror("Błąd msgget (połączenie)");
        exit(1);
    }
    return msg_id;
}

int polacz_kolejke_worker(void) {
    key_t klucz = utworz_klucz(IPC_KEY_MSG_WORKER);
    int msg_id = msgget(klucz, 0600);
    if (msg_id == -1) {
        perror("Błąd msgget worker (połączenie)");
        exit(1);
    }
    return msg_id;
}

void usun_kolejke(int msg_id) {
    if (msgctl(msg_id, IPC_RMID, NULL) == -1) {
        perror("Błąd msgctl IPC_RMID");
    }
}

bool wyslij_komunikat(int msg_id, Message* msg) {
    while (msgsnd(msg_id, msg, MSG_SIZE, 0) == -1) {
        if (errno == EINTR) continue;
        perror("Błąd msgsnd");
        return false;
    }
    return true;
}

bool wyslij_komunikat_nowait(int msg_id, Message* msg) {
    if (msgsnd(msg_id, msg, MSG_SIZE, IPC_NOWAIT) == -1) {
        if (errno == EAGAIN) return false; // Kolejka pełna
        if (errno != EINTR) {
            perror("Błąd msgsnd nowait");
        }
        return false;
    }
    return true;
}

bool odbierz_komunikat(int msg_id, Message* msg, long mtype, bool blocking) {
    int flags = blocking ? 0 : IPC_NOWAIT;
    
    while (msgrcv(msg_id, msg, MSG_SIZE, mtype, flags) == -1) {
        if (errno == EINTR) continue;
        if (errno == ENOMSG && !blocking) return false;
        if (errno == EAGAIN) return false;
        perror("Błąd msgrcv");
        return false;
    }
    return true;
}

bool odbierz_komunikat_timeout(int msg_id, Message* msg, long mtype, int timeout_ms) {
    int elapsed = 0;
    int step = 1; // 1ms
    
    while (elapsed < timeout_ms) {
        if (msgrcv(msg_id, msg, MSG_SIZE, mtype, IPC_NOWAIT) != -1) {
            return true;
        }
        if (errno != ENOMSG && errno != EAGAIN && errno != EINTR) {
            perror("Błąd msgrcv timeout");
            return false;
        }
        usleep(step * 1000);
        elapsed += step;
    }
    return false;
}

// ==== FUNKCJE POMOCNICZE ====
const char* nazwa_biletu(TicketType type) {
    switch (type) {
        case TICKET_SINGLE: return "JEDNORAZOWY";
        case TICKET_TK1:    return "CZASOWY TK1 (1h)";
        case TICKET_TK2:    return "CZASOWY TK2 (2h)";
        case TICKET_TK3:    return "CZASOWY TK3 (3h)";
        case TICKET_DAILY:  return "DZIENNY";
        default:            return "NIEZNANY";
    }
}

const char* nazwa_trasy(TrailType trail) {
    switch (trail) {
        case TRAIL_T1: return "T1 (łatwa)";
        case TRAIL_T2: return "T2 (średnia)";
        case TRAIL_T3: return "T3 (trudna)";
        default:       return "NIEZNANA";
    }
}

int cena_biletu(TicketType type, bool discount) {
    int base_price;
    switch (type) {
        case TICKET_SINGLE: base_price = PRICE_SINGLE; break;
        case TICKET_TK1:    base_price = PRICE_TK1; break;
        case TICKET_TK2:    base_price = PRICE_TK2; break;
        case TICKET_TK3:    base_price = PRICE_TK3; break;
        case TICKET_DAILY:  base_price = PRICE_DAILY; break;
        default:            base_price = 0; break;
    }
    
    if (discount) {
        return base_price - (base_price * DISCOUNT_PERCENT / 100);
    }
    return base_price;
}

int czas_waznosci(TicketType type) {
    switch (type) {
        case TICKET_TK1:    return TK1_DURATION;
        case TICKET_TK2:    return TK2_DURATION;
        case TICKET_TK3:    return TK3_DURATION;
        case TICKET_SINGLE: return 0; // Jednorazowy
        case TICKET_DAILY:  return 0; // Cały dzień
        default:            return 0;
    }
}

void czysc_zasoby(void) {
    key_t klucz;
    int id;
    
    // Semafory
    klucz = ftok(IPC_KEY_PATH, IPC_KEY_SEM);
    if (klucz != -1) {
        id = semget(klucz, 0, 0);
        if (id != -1) semctl(id, 0, IPC_RMID);
    }
    
    // Pamięć dzielona
    klucz = ftok(IPC_KEY_PATH, IPC_KEY_SHM);
    if (klucz != -1) {
        id = shmget(klucz, 0, 0);
        if (id != -1) shmctl(id, IPC_RMID, NULL);
    }
    
    // Kolejka główna
    klucz = ftok(IPC_KEY_PATH, IPC_KEY_MSG);
    if (klucz != -1) {
        id = msgget(klucz, 0);
        if (id != -1) msgctl(id, IPC_RMID, NULL);
    }
    
    // Kolejka pracowników
    klucz = ftok(IPC_KEY_PATH, IPC_KEY_MSG_WORKER);
    if (klucz != -1) {
        id = msgget(klucz, 0);
        if (id != -1) msgctl(id, IPC_RMID, NULL);
    }
}

void blad_krytyczny(const char* msg) {
    fprintf(stderr, ANSI_RED "BŁĄD KRYTYCZNY: %s" ANSI_RESET "\n", msg);
    perror("Szczegóły");
    exit(1);
}

void blad_ostrzezenie(const char* msg) {
    fprintf(stderr, ANSI_YELLOW "OSTRZEŻENIE: %s" ANSI_RESET "\n", msg);
}
