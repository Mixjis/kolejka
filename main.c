#include "common.h"

pid_t *child_pids = NULL;
int num_children = 0;

volatile sig_atomic_t is_sim_running = 1;

void create_ipc_resources();
void main_signal_handler(int sig);

int main(void) {
    atexit(cleanup_ipc);
    signal(SIGINT, main_signal_handler);
    signal(SIGALRM, main_signal_handler);
    signal(SIGTERM, main_signal_handler);

    fclose(fopen("kolej_log.txt", "w"));

    create_ipc_resources();

    //stan symulacji
    state->is_running = 1;
    state->start_time = time(NULL);
    state->end_time = state->start_time + SIM_DURATION;
    
    //wspólne kolejki
    memset(&state->waiting_tourists_down, 0, sizeof(SharedQueue));
    memset(&state->waiting_tourists_up, 0, sizeof(SharedQueue));

    child_pids = malloc(sizeof(pid_t) * (NUM_TOURISTS + 4));
    if (!child_pids) {
        perror("malloc child_pids");
        exit(1);
    }
    
    printf("SYSTEM: Uruchamianie procesów potomnych...\n");
    
    char* argv_logger[] = {"./logger", NULL};
    char* argv_cashier[] = {"./cashier", NULL};
    char* argv_worker_d[] = {"./worker", "down", NULL};
    char* argv_worker_u[] = {"./worker", "up", NULL};

    pid_t pid;
    
    if ((pid = fork()) == 0) { execv("./logger", argv_logger); perror("execv logger"); exit(1); }
    child_pids[num_children++] = pid;
    
    if ((pid = fork()) == 0) { execv("./cashier", argv_cashier); perror("execv cashier"); exit(1); }
    child_pids[num_children++] = pid;
    
    if ((pid = fork()) == 0) { execv("./worker", argv_worker_d); perror("execv worker down"); exit(1); }
    child_pids[num_children++] = pid;
    
    if ((pid = fork()) == 0) { execv("./worker", argv_worker_u); perror("execv worker up"); exit(1); }
    child_pids[num_children++] = pid;

    // Tworzenie turystów
    for (int i = 1; i <= NUM_TOURISTS; i++) {
        char id_str[12];
        sprintf(id_str, "%d", i);
        char* argv_tourist[] = {"./tourist", id_str, NULL};
        if ((pid = fork()) == 0) { execv("./tourist", argv_tourist); perror("execv tourist"); exit(1); }
        child_pids[num_children++] = pid;
        //usleep(20000);
    }
    
    printf("SYSTEM: Wszystkie procesy uruchomione. Symulacja potrwa %d sekund.\n", SIM_DURATION);

    // alarm na koniec symulacji
    alarm(SIM_DURATION);
    
    // Czekanie na sygnał (SIGALRM lub SIGINT)
    while(is_sim_running) {
        pause();
    }

    printf("\nSYSTEM: Koniec czasu symulacji lub SIGINT. Zamykanie...\n");
    state->is_running = 0;
    
    //usleep(100000);

    // Zabicie wszystkich pozostałych
    for (int i = 0; i < num_children; i++) {
        kill(child_pids[i], SIGTERM);
    }
    // Czekanie na zakończenie wszystkich potomków
    while(wait(NULL) > 0 || errno != ECHILD);
    
    printf("SYSTEM: Wszystkie procesy potomne zakończone.\n");
    
    // Raport końcowy
    sem_wait(sem_state_mutex_id,0);
    printf("\n================ RAPORT KOŃCOWY SYMULACJI ================\n");
    printf("\n1. SPRZEDAŻ BILETÓW:\n");
    printf("   - Jednorazowe (SINGLE):      %d szt.\n", stats->single_sold);
    printf("   - Czasowe TK1 (1h):          %d szt.\n", stats->tk1_sold);
    printf("   - Czasowe TK2 (2h):          %d szt.\n", stats->tk2_sold);
    printf("   - Czasowe TK3 (3h):          %d szt.\n", stats->tk3_sold);
    printf("   - Dzienne (DAILY):           %d szt.\n", stats->daily_sold);
    printf("   RAZEM SPRZEDANYCH BILETÓW:   %d\n", 
        stats->single_sold + stats->tk1_sold + stats->tk2_sold + stats->tk3_sold + stats->daily_sold);
    
    printf("\n2. STATYSTYKA PRZEJAZDÓW:\n");
    printf("   - Łączna liczba przejazdów:  %d\n", stats->total_rides);
    printf("   - Rowerzyści (bikers):       %d osób\n", stats->bikers);
    printf("   - Piesi (walkers):           %d osób\n", stats->walkers);
    printf("   - Średnio osób na przejazd:  %.2f\n", 
        stats->total_rides > 0 ? (float)(stats->bikers + stats->walkers) / stats->total_rides : 0.0);
    
    printf("\n3. KATEGORIE SPECJALNE:\n");
    printf("   - Obsłużeni VIP:             %d osób\n", stats->vip_served);
    printf("   - Dzieci z opiekunem:        %d osób\n", stats->with_guardian);
    printf("   - Wykorzystane miejsca:      %d/%d (%.1f%%)\n", 
        stats->bikers + stats->walkers, MAX_BUSY_CHAIRS * CHAIR_CAPACITY,
        stats->total_rides > 0 ? 100.0f * (stats->bikers + stats->walkers) / (MAX_BUSY_CHAIRS * CHAIR_CAPACITY) : 0.0);
    
    printf("\n4. TRASY ZJAZDOWE:\n");
    printf("   - Trasa T1 (łatwa):          %d turystów\n", stats->route_t1);
    printf("   - Trasa T2 (średnia):        %d turystów\n", stats->route_t2);
    printf("   - Trasa T3 (trudna):         %d turystów\n", stats->route_t3);
    
    printf("\n============================================================\n");
    printf("Symulacja trwała %d sekund. Załadowano %d turystów.\n", SIM_DURATION, NUM_TOURISTS);
    printf("============================================================\n\n");
    sem_signal(sem_state_mutex_id,0);

    free(child_pids);

    return 0;
}

void main_signal_handler(int sig) {
    if (sig == SIGALRM) {
        printf("\nSYSTEM: Otrzymano SIGALRM. Koniec czasu.\n");
    } else if (sig == SIGINT || sig == SIGTERM) {
        printf("\nSYSTEM: Otrzymano sygnał %s. Zamykanie.\n", sig == SIGINT ? "SIGINT" : "SIGTERM");
    }
    is_sim_running = 0;
}


void create_ipc_resources() {
    key_t key_msg = ftok(".", 'Q');
    msg_queue_id = msgget(key_msg, IPC_CREAT | 0660);

    key_t key_state = ftok(".", 'S');
    shm_state_id = shmget(key_state, sizeof(StationState), IPC_CREAT | 0660);
    state = (StationState *)shmat(shm_state_id, NULL, 0);

    key_t key_tickets = ftok(".", 'K');
    shm_tickets_id = shmget(key_tickets, sizeof(Ticket) * (NUM_TOURISTS + 1), IPC_CREAT | 0660);
    tickets = (Ticket *)shmat(shm_tickets_id, NULL, 0);

    key_t key_stats = ftok(".", 'T');
    shm_stats_id = shmget(key_stats, sizeof(DailyStats), IPC_CREAT | 0660);
    stats = (DailyStats *)shmat(shm_stats_id, NULL, 0);
    
    memset(state, 0, sizeof(StationState));
    memset(stats, 0, sizeof(DailyStats));
    memset(tickets, 0, sizeof(Ticket) * (NUM_TOURISTS + 1));

    sem_state_mutex_id = semget(ftok(".", 'M'), 1, IPC_CREAT | 0660);
    semctl(sem_state_mutex_id, 0, SETVAL, 1);

    sem_entry_id = semget(ftok(".", 'E'), ENTRY_GATES, IPC_CREAT | 0660);
    for(int i=0; i<ENTRY_GATES; i++) semctl(sem_entry_id, i, SETVAL, 1);

    sem_station_id = semget(ftok(".", 'A'), 1, IPC_CREAT | 0660);
    semctl(sem_station_id, 0, SETVAL, MAX_STATION_CAPACITY);
    
    sem_platform_id = semget(ftok(".", 'P'), PLATFORM_GATES, IPC_CREAT | 0660);
    for(int i=0; i<PLATFORM_GATES; i++) semctl(sem_platform_id, i, SETVAL, 1);

    sem_ticket_bought_id = semget(ftok(".", 'B'), NUM_TOURISTS, IPC_CREAT | 0660);
    for(int i=0; i<NUM_TOURISTS; i++) semctl(sem_ticket_bought_id, i, SETVAL, 0);

    sem_chair_queue_id = semget(ftok(".", 'C'), 1, IPC_CREAT | 0660);
    semctl(sem_chair_queue_id, 0, SETVAL, 1);

    sem_chair_loaded_id = semget(ftok(".", 'L'), 1, IPC_CREAT | 0660);
    semctl(sem_chair_loaded_id, 0, SETVAL, 0);

    sem_exit_id = semget(ftok(".", 'X'), EXIT_ROUTES, IPC_CREAT | 0660);
    for(int i=0; i<EXIT_ROUTES; i++) semctl(sem_exit_id, i, SETVAL, 1);

    printf("SYSTEM: Utworzono zasoby IPC.\n");
}