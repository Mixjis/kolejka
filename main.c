#include "common.h"
#include <stdarg.h>

// Kolorowe logowanie dla procesu glownego
static void sys_log(const char *format, ...) {
    time_t now = time(NULL);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    printf(ANSI_BOLD_WHITE "[%s] [SYSTEM|PID:%d] ", time_str, getpid());

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf(ANSI_RESET);
}

pid_t *child_pids = NULL;
int num_children = 0;
volatile sig_atomic_t is_sim_running = 1;
volatile sig_atomic_t sigint_count = 0;

void create_ipc_resources();
void main_signal_handler(int sig);
void graceful_shutdown();
void generate_daily_report();

int main(void) {
    process_role = ROLE_MAIN;
    atexit(cleanup_ipc);

    struct sigaction sa;
    sa.sa_handler = main_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    // Wyczysc plik logu
    fclose(fopen("kolej_log.txt", "w"));

    // Utworz FIFO do komunikacji miedzy pracownikami
    unlink(FIFO_WORKER_PATH);
    if (mkfifo(FIFO_WORKER_PATH, 0660) == -1) {
        perror("mkfifo");
        // Kontynuuj mimo bledu - FIFO moze juz istniec
    }

    create_ipc_resources();

    state->is_running = 1;
    state->is_closing = 0;
    state->is_paused = 0;
    state->start_time = time(NULL);
    state->end_time = state->start_time + SIM_DURATION;
    state->worker_down_pid = 0;
    state->worker_up_pid = 0;
    state->next_family_id = 1;
    state->pass_log_count = 0;
    state->people_on_top = 0;
    memset(&state->waiting_tourists_down, 0, sizeof(SharedQueue));

    // +4 = logger, cashier, worker1, worker2
    child_pids = malloc(sizeof(pid_t) * (NUM_TOURISTS + 4));
    if (!child_pids) {
        perror("malloc child_pids");
        exit(1);
    }

    sys_log("Uruchamianie procesow potomnych...\n");

    char* argv_logger[] = {"./logger", NULL};
    char* argv_cashier[] = {"./cashier", NULL};
    char* argv_worker_d[] = {"./worker", NULL};
    char* argv_worker_u[] = {"./worker2", NULL};

    pid_t pid;

    // Logger
    if ((pid = fork()) == 0) {
        execv("./logger", argv_logger);
        perror("execv logger");
        exit(1);
    } else if (pid == -1) {
        perror("fork logger");
        exit(1);
    }
    child_pids[num_children++] = pid;

    // Kasjer
    if ((pid = fork()) == 0) {
        execv("./cashier", argv_cashier);
        perror("execv cashier");
        exit(1);
    } else if (pid == -1) {
        perror("fork cashier");
        exit(1);
    }
    child_pids[num_children++] = pid;

    // Pracownik1 (stacja dolna)
    if ((pid = fork()) == 0) {
        execv("./worker", argv_worker_d);
        perror("execv worker");
        exit(1);
    } else if (pid == -1) {
        perror("fork worker");
        exit(1);
    }
    child_pids[num_children++] = pid;

    // Pracownik2 (stacja gorna)
    if ((pid = fork()) == 0) {
        execv("./worker2", argv_worker_u);
        perror("execv worker2");
        exit(1);
    } else if (pid == -1) {
        perror("fork worker2");
        exit(1);
    }
    child_pids[num_children++] = pid;

    sys_log("Tworzenie %d turystow...\n", NUM_TOURISTS);

    for (int i = 1; i <= NUM_TOURISTS; i++) {
        char id_str[12];
        sprintf(id_str, "%d", i);
        char* argv_tourist[] = {"./tourist", id_str, NULL};

        int retry_count = 0;
        const int MAX_RETRIES = 5;

        while (retry_count < MAX_RETRIES) {
            pid = fork();
            if (pid == 0) {
                execv("./tourist", argv_tourist);
                perror("execv tourist");
                exit(1);
            } else if (pid > 0) {
                child_pids[num_children++] = pid;
                break;
            } else {
                if (errno == EAGAIN) {
                    fprintf(stderr, "SYSTEM: fork() EAGAIN dla turysty %d, retry %d/%d\n",
                            i, retry_count + 1, MAX_RETRIES);
                    usleep(10000);
                    retry_count++;
                } else {
                    perror("fork tourist");
                    break;
                }
            }
        }

        // Co 10 turystow - pauza (skalowana z liczba turystow)
        if (i % 10 == 0) {
            usleep(NUM_TOURISTS > 100 ? 50000 : 10000);
        }
    }

    sys_log("Utworzono %d/%d procesow turystow.\n", num_children - 4, NUM_TOURISTS);
    sys_log("Symulacja uruchomiona. Czas zamkniecia bramek: %d s, calkowity: %d s.\n",
           CLOSING_TIME, SIM_DURATION);

    // Ustaw alarm na czas zamkniecia bramek
    alarm(CLOSING_TIME);

    while (is_sim_running) {
        pause();
    }

    graceful_shutdown();
    fflush(stdout);
    usleep(500000); // poczekaj az wszystkie procesy potomne zakoncza wypisywanie
    generate_daily_report();

    free(child_pids);
    return 0;
}

void main_signal_handler(int sig) {
    if (sig == SIGALRM) {
        printf("\n" ANSI_BOLD_YELLOW "[SYSTEM] Czas zamkniecia (Tk). Zamykanie bramek." ANSI_RESET "\n");
        is_sim_running = 0;
    } else if (sig == SIGINT || sig == SIGTERM) {
        sigint_count++;
        printf("\n" ANSI_BOLD_RED "[SYSTEM] Otrzymano %s. Zamykanie." ANSI_RESET "\n", sig == SIGINT ? "SIGINT" : "SIGTERM");
        is_sim_running = 0;
        if (sigint_count >= 2) {
            printf("\n" ANSI_BOLD_RED "[SYSTEM] Wymuszone wyjscie (podwojne Ctrl+C)!" ANSI_RESET "\n");
            for (int i = 0; i < num_children; i++) {
                if (child_pids[i] > 0) kill(child_pids[i], SIGKILL);
            }
            cleanup_ipc();
            exit(1);
        }
    }
}

void graceful_shutdown() {
    printf("\n" ANSI_BOLD_YELLOW "[SYSTEM] Rozpoczynam zamykanie kolei..." ANSI_RESET "\n");

    // Zamknij bramki - karnety przestaja dzialac
    sem_wait_op(sem_state_mutex_id, 0);
    state->is_closing = 1;
    sem_signal_op(sem_state_mutex_id, 0);

    sys_log("Bramki wejsciowe zamkniete. Karnety nieaktywne.\n");
    sys_log("Oczekiwanie na przetransportowanie osob z peronu...\n");

    // Czekaj az kolejka i krzesla sie oproznia (peron pusty)
    int wait_iterations = 0;
    const int MAX_WAIT = 30;

    while (wait_iterations < MAX_WAIT && sigint_count < 2) {
        sem_wait_op(sem_state_mutex_id, 0);
        int pop = state->station_population;
        int busy = state->busy_chairs;
        int queue_count = state->waiting_tourists_down.count;
        int on_top = state->people_on_top;
        sem_signal_op(sem_state_mutex_id, 0);

        if (wait_iterations % 3 == 0) {
            sys_log("Status - Stacja:%d, Krzesla:%d/%d, Kolejka:%d, Na gorze:%d\n",
                   pop, busy, MAX_BUSY_CHAIRS, queue_count, on_top);
        }

        // Warunek: kolejka pusta i krzesla wolne = wszyscy z peronu przetransportowani
        if (busy == 0 && queue_count == 0) {
            sys_log("Peron pusty, wszyscy przetransportowani.\n");
            break;
        }

        sleep(1);
        wait_iterations++;
    }

    // Wg opisu: po 3 sekundach kolej ma zostac wylaczona
    sys_log("Odliczanie przed wylaczeniem kolei...\n");
    sleep(1);

    // Zatrzymaj kolej
    sem_wait_op(sem_state_mutex_id, 0);
    state->is_running = 0;
    sem_signal_op(sem_state_mutex_id, 0);

    sys_log("Kolej WYLACZONA.\n");

    // Wyslij SIGTERM do worker1 (stacja dolna) i kasjera - kolej juz nie dziala
    // child_pids[1] = cashier, child_pids[2] = worker1
    if (num_children > 1 && child_pids[1] > 0) {
        kill(child_pids[1], SIGTERM);
    }
    if (num_children > 2 && child_pids[2] > 0) {
        kill(child_pids[2], SIGTERM);
    }

    // Czekaj az turyści na gorze zjada/zejda (worker2 nadal ich obsluguje)
    sys_log("Oczekiwanie na turystow na gorze...\n");
    int top_wait = 0;
    const int MAX_TOP_WAIT = ROUTE_TIME_T3 + RIDE_DURATION + 5; // max czas zjazdu + margines

    while (top_wait < MAX_TOP_WAIT && sigint_count < 2) {
        sem_wait_op(sem_state_mutex_id, 0);
        int on_top = state->people_on_top;
        int pop = state->station_population;
        sem_signal_op(sem_state_mutex_id, 0);

        if (on_top == 0 && pop == 0) {
            sys_log("Wszyscy turyści opuscili system.\n");
            break;
        }

        if (top_wait % 3 == 0) {
            sys_log("Oczekiwanie - Na gorze:%d, Stacja:%d\n", on_top, pop);
        }

        sleep(1);
        top_wait++;
    }

    // Teraz wyslij SIGTERM do worker2 (stacja gorna) - turyści juz zjechali
    // child_pids[3] = worker2
    if (num_children > 3 && child_pids[3] > 0) {
        kill(child_pids[3], SIGTERM);
    }

    // Wyslij SIGTERM do turystow ktorzy jeszcze zyja (np. czekajacy na kase)
    for (int i = 4; i < num_children; i++) {
        if (child_pids[i] > 0) {
            kill(child_pids[i], SIGTERM);
        }
    }

    // Wyslij SIGTERM do loggera
    // child_pids[0] = logger
    if (num_children > 0 && child_pids[0] > 0) {
        kill(child_pids[0], SIGTERM);
    }

    sys_log("Oczekiwanie na zakonczenie procesow...\n");

    // Czekaj na zakonczenie WSZYSTKICH procesow potomnych (blokujace waitpid)
    int max_wait_sec = MAX_TOP_WAIT + 5;
    int waited = 0;
    while (waited < max_wait_sec * 2) {  // sprawdzaj co 500ms
        int status;
        pid_t finished = waitpid(-1, &status, WNOHANG);
        if (finished <= 0) {
            if (errno == ECHILD) break;  // brak procesow potomnych
        }
        usleep(500000);
        waited++;
    }

    sys_log("Procesy zakonczone.\n");
}

// Generowanie raportu koncowego (zapis do kolej_log.txt)
void generate_daily_report() {
    sem_wait_op(sem_state_mutex_id, 0);

    int total_sold = stats->single_sold + stats->tk1_sold + stats->tk2_sold +
                     stats->tk3_sold + stats->daily_sold;

    // Raport na ekran
    printf(ANSI_BOLD_MAGENTA "\n============================================================\n");
    printf("         RAPORT KONCOWY SYMULACJI KOLEI LINOWEJ\n");
    printf("============================================================\n" ANSI_RESET);
    printf(ANSI_MAGENTA "\n1. SPRZEDAZ BILETOW:\n" ANSI_RESET);
    printf("   Jednorazowe (SINGLE):     %d szt.\n", stats->single_sold);
    printf("   Czasowe TK1 (1h):         %d szt.\n", stats->tk1_sold);
    printf("   Czasowe TK2 (2h):         %d szt.\n", stats->tk2_sold);
    printf("   Czasowe TK3 (3h):         %d szt.\n", stats->tk3_sold);
    printf("   Dzienne (DAILY):          %d szt.\n", stats->daily_sold);
    printf(ANSI_BOLD_WHITE "   RAZEM:                    %d szt.\n" ANSI_RESET, total_sold);
    printf(ANSI_MAGENTA "\n2. STATYSTYKI PRZEJAZDOW:\n" ANSI_RESET);
    printf("   Odjazdy krzeselek:        %d\n", stats->total_rides);
    printf("   Przewiezione osoby:       %d\n", stats->total_people);
    printf("   - Rowerzyści:             %d\n", stats->bikers);
    printf("   - Piesi:                  %d\n", stats->walkers);
    if (stats->total_rides > 0) {
        printf("   Sr. osob/krzeslo:         %.2f\n",
               (float)stats->total_people / stats->total_rides);
    }
    printf(ANSI_MAGENTA "\n3. KATEGORIE SPECJALNE:\n" ANSI_RESET);
    printf("   VIP obsluzeni:            %d\n", stats->vip_served);
    printf("   Dzieci z opiekunem:       %d\n", stats->with_guardian);
    printf("   Odrzuceni (wygasly):      %d\n", stats->rejected_expired);
    printf(ANSI_MAGENTA "\n4. TRASY ZJAZDOWE:\n" ANSI_RESET);
    printf("   T1 (latwa, %ds):          %d turystow\n", ROUTE_TIME_T1, stats->route_t1);
    printf("   T2 (srednia, %ds):        %d turystow\n", ROUTE_TIME_T2, stats->route_t2);
    printf("   T3 (trudna, %ds):         %d turystow\n", ROUTE_TIME_T3, stats->route_t3);
    printf(ANSI_MAGENTA "\n5. REJESTRACJA PRZEJSC BRAMKOWYCH:\n" ANSI_RESET);
    printf("   Zarejestrowanych przejsc: %d\n", state->pass_log_count);
    printf(ANSI_BOLD_MAGENTA "\n============================================================\n\n" ANSI_RESET);

    // Zapis raportu do kolej_log.txt (dopisanie na koniec)
    FILE *report = fopen("kolej_log.txt", "a");
    if (report) {
        fprintf(report, "\n============================================================\n");
        fprintf(report, "    RAPORT DZIENNY KOLEI LINOWEJ\n");
        fprintf(report, "============================================================\n\n");

        fprintf(report, "SPRZEDAZ BILETOW:\n");
        fprintf(report, "  JEDNORAZOWY: %d\n", stats->single_sold);
        fprintf(report, "  CZASOWY_1H:  %d\n", stats->tk1_sold);
        fprintf(report, "  CZASOWY_2H:  %d\n", stats->tk2_sold);
        fprintf(report, "  CZASOWY_3H:  %d\n", stats->tk3_sold);
        fprintf(report, "  DZIENNY:     %d\n", stats->daily_sold);
        fprintf(report, "  RAZEM:       %d\n\n", total_sold);

        fprintf(report, "PRZEJAZDY:\n");
        fprintf(report, "  Odjazdy krzeselek: %d\n", stats->total_rides);
        fprintf(report, "  Przewiezione osoby: %d\n", stats->total_people);
        fprintf(report, "  Rowerzyści: %d, Piesi: %d\n", stats->bikers, stats->walkers);
        fprintf(report, "  VIP: %d, Z opiekunem: %d\n\n", stats->vip_served, stats->with_guardian);

        fprintf(report, "TRASY ZJAZDOWE:\n");
        fprintf(report, "  T1 (latwa):   %d\n", stats->route_t1);
        fprintf(report, "  T2 (srednia): %d\n", stats->route_t2);
        fprintf(report, "  T3 (trudna):  %d\n\n", stats->route_t3);

        fprintf(report, "REJESTR PRZEJSC BRAMKOWYCH (id_karnetu - godzina - nr_przejazdu):\n");
        fprintf(report, "------------------------------------------------------------\n");
        for (int i = 0; i < state->pass_log_count && i < MAX_PASS_LOG; i++) {
            PassLogEntry *e = &state->pass_log[i];
            char time_str[20];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&e->timestamp));
            const char *type_name;
            switch (e->ticket_type) {
                case SINGLE: type_name = "JEDNORAZOWY"; break;
                case TK1:    type_name = "CZASOWY_1H"; break;
                case TK2:    type_name = "CZASOWY_2H"; break;
                case TK3:    type_name = "CZASOWY_3H"; break;
                case DAILY:  type_name = "DZIENNY"; break;
                default:     type_name = "?"; break;
            }
            fprintf(report, "  Karnet#%03d (%s) - %s - przejazd #%d\n",
                    e->tourist_id, type_name, time_str, e->ride_number);
        }

        fprintf(report, "\n============================================================\n");
        fclose(report);
        sys_log("Raport dopisany do pliku 'kolej_log.txt'.\n");
    } else {
        perror("fopen kolej_log.txt");
    }

    // === RAPORT KARNETOW (osobny plik - wymog: rejestracja przejsc + podsumowanie zjazdow) ===
    FILE *karnet_report = fopen("raport_karnetow.txt", "w");
    if (karnet_report) {
        fprintf(karnet_report, "============================================================\n");
        fprintf(karnet_report, "  RAPORT KARNETOW - REJESTRACJA PRZEJSC BRAMKOWYCH\n");
        fprintf(karnet_report, "============================================================\n\n");

        fprintf(karnet_report, "REJESTR PRZEJSC (id karnetu - godzina - nr przejazdu):\n");
        fprintf(karnet_report, "------------------------------------------------------------\n");
        for (int i = 0; i < state->pass_log_count && i < MAX_PASS_LOG; i++) {
            PassLogEntry *e = &state->pass_log[i];
            char time_str[20];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&e->timestamp));
            const char *type_name;
            switch (e->ticket_type) {
                case SINGLE: type_name = "JEDNORAZOWY"; break;
                case TK1:    type_name = "CZASOWY_1H"; break;
                case TK2:    type_name = "CZASOWY_2H"; break;
                case TK3:    type_name = "CZASOWY_3H"; break;
                case DAILY:  type_name = "DZIENNY"; break;
                default:     type_name = "?"; break;
            }
            fprintf(karnet_report, "  Karnet#%03d (%s) - %s - przejazd #%d\n",
                    e->tourist_id, type_name, time_str, e->ride_number);
        }

        // Podsumowanie liczby zjazdow per turysta/karnet
        fprintf(karnet_report, "\n============================================================\n");
        fprintf(karnet_report, "  PODSUMOWANIE ZJAZDOW PER TURYSTA/KARNET\n");
        fprintf(karnet_report, "============================================================\n");
        fprintf(karnet_report, "  %-8s %-14s %-10s %s\n", "ID", "TYP KARNETU", "PRZEJAZDY", "STATUS");
        fprintf(karnet_report, "------------------------------------------------------------\n");

        for (int i = 1; i <= NUM_TOURISTS; i++) {
            Ticket *t = &tickets[i];
            if (t->validation_count > 0) {
                const char *type_name;
                switch (t->type) {
                    case SINGLE: type_name = "JEDNORAZOWY"; break;
                    case TK1:    type_name = "CZASOWY_1H"; break;
                    case TK2:    type_name = "CZASOWY_2H"; break;
                    case TK3:    type_name = "CZASOWY_3H"; break;
                    case DAILY:  type_name = "DZIENNY"; break;
                    default:     type_name = "?"; break;
                }
                const char *status = t->is_valid ? "AKTYWNY" : "NIEAKTYWNY";
                fprintf(karnet_report, "  %-8d %-14s %-10d %s\n",
                        i, type_name, t->validation_count, status);
            }
        }

        fprintf(karnet_report, "\n============================================================\n");
        fclose(karnet_report);
        sys_log("Raport karnetow zapisany do 'raport_karnetow.txt'.\n");
    } else {
        perror("fopen raport_karnetow.txt");
    }

    sem_signal_op(sem_state_mutex_id, 0);
}

void create_ipc_resources() {
    key_t key_msg = ftok(".", 'Q');
    if (key_msg == -1) { perror("ftok Q"); exit(1); }
    msg_queue_id = msgget(key_msg, IPC_CREAT | 0660);
    if (msg_queue_id == -1) { perror("msgget create"); exit(1); }

    key_t key_state = ftok(".", 'S');
    if (key_state == -1) { perror("ftok S"); exit(1); }
    shm_state_id = shmget(key_state, sizeof(StationState), IPC_CREAT | 0660);
    if (shm_state_id == -1) { perror("shmget state"); exit(1); }
    state = (StationState *)shmat(shm_state_id, NULL, 0);
    if (state == (void *)-1) { perror("shmat state"); exit(1); }

    key_t key_tickets = ftok(".", 'K');
    if (key_tickets == -1) { perror("ftok K"); exit(1); }
    shm_tickets_id = shmget(key_tickets, sizeof(Ticket) * (NUM_TOURISTS + 1), IPC_CREAT | 0660);
    if (shm_tickets_id == -1) { perror("shmget tickets"); exit(1); }
    tickets = (Ticket *)shmat(shm_tickets_id, NULL, 0);
    if (tickets == (void *)-1) { perror("shmat tickets"); exit(1); }

    key_t key_stats = ftok(".", 'T');
    if (key_stats == -1) { perror("ftok T"); exit(1); }
    shm_stats_id = shmget(key_stats, sizeof(DailyStats), IPC_CREAT | 0660);
    if (shm_stats_id == -1) { perror("shmget stats"); exit(1); }
    stats = (DailyStats *)shmat(shm_stats_id, NULL, 0);
    if (stats == (void *)-1) { perror("shmat stats"); exit(1); }

    memset(state, 0, sizeof(StationState));
    memset(stats, 0, sizeof(DailyStats));
    memset(tickets, 0, sizeof(Ticket) * (NUM_TOURISTS + 1));

    sem_state_mutex_id = semget(ftok(".", 'M'), 1, IPC_CREAT | 0660);
    if (sem_state_mutex_id == -1) { perror("semget M"); exit(1); }
    semctl(sem_state_mutex_id, 0, SETVAL, 1);

    sem_entry_id = semget(ftok(".", 'E'), ENTRY_GATES, IPC_CREAT | 0660);
    if (sem_entry_id == -1) { perror("semget E"); exit(1); }
    for (int i = 0; i < ENTRY_GATES; i++) {
        semctl(sem_entry_id, i, SETVAL, 1);
    }

    sem_station_id = semget(ftok(".", 'A'), 1, IPC_CREAT | 0660);
    if (sem_station_id == -1) { perror("semget A"); exit(1); }
    semctl(sem_station_id, 0, SETVAL, MAX_STATION_CAPACITY);

    sem_platform_id = semget(ftok(".", 'P'), PLATFORM_GATES, IPC_CREAT | 0660);
    if (sem_platform_id == -1) { perror("semget P"); exit(1); }
    for (int i = 0; i < PLATFORM_GATES; i++) {
        semctl(sem_platform_id, i, SETVAL, 1);
    }

    sem_ticket_bought_id = semget(ftok(".", 'B'), NUM_TOURISTS, IPC_CREAT | 0660);
    if (sem_ticket_bought_id == -1) { perror("semget B"); exit(1); }
    for (int i = 0; i < NUM_TOURISTS; i++) {
        semctl(sem_ticket_bought_id, i, SETVAL, 0);
    }

    sem_chair_queue_id = semget(ftok(".", 'C'), 1, IPC_CREAT | 0660);
    if (sem_chair_queue_id == -1) { perror("semget C"); exit(1); }
    semctl(sem_chair_queue_id, 0, SETVAL, 1);

    sem_chair_loaded_id = semget(ftok(".", 'L'), 1, IPC_CREAT | 0660);
    if (sem_chair_loaded_id == -1) { perror("semget L"); exit(1); }
    semctl(sem_chair_loaded_id, 0, SETVAL, 0);

    sem_exit_id = semget(ftok(".", 'X'), EXIT_ROUTES, IPC_CREAT | 0660);
    if (sem_exit_id == -1) { perror("semget X"); exit(1); }
    for (int i = 0; i < EXIT_ROUTES; i++) {
        semctl(sem_exit_id, i, SETVAL, 1);
    }

    sem_worker_wakeup_down_id = semget(ftok(".", 'D'), 1, IPC_CREAT | 0660);
    if (sem_worker_wakeup_down_id == -1) { perror("semget D"); exit(1); }
    semctl(sem_worker_wakeup_down_id, 0, SETVAL, 0);

    sys_log("Zasoby IPC utworzone pomyslnie.\n");
}
