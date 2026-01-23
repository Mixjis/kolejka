#include "common.h"

int tourist_id;
int age;
TouristType type;
int is_vip;
int num_children = 0;
int family_id = 0;
pthread_t child_threads[MAX_CHILDREN_PER_GUARDIAN];

typedef struct {
    int child_num;
    int age;
} ChildData;

volatile sig_atomic_t tourist_running = 1;
volatile sig_atomic_t force_exit = 0;

void tourist_sigterm_handler(int sig) {
    (void)sig;
    tourist_running = 0;
    force_exit = 1;
}

// Watek dziecka (pod opieka dorosleg)
void* child_thread_func(void* arg) {
    ChildData* data = (ChildData*)arg;
    log_msg("DZIECKO %d (family:%d, wiek:%d): Watek uruchomiony, pod opieka.",
            data->child_num, family_id, data->age);

    // Czekaj az opiekun kupi bilet
    while (!force_exit) {
        if (sem_trywait_op(sem_ticket_bought_id, tourist_id - 1) == 0) {
            log_msg("DZIECKO %d (family:%d): Rodzina gotowa, podazam za opiekunem.",
                    data->child_num, family_id);
            break;
        }
        usleep(100000);
        if (force_exit) break;
    }

    free(data);
    return NULL;
}

void create_family_threads() {
    if (num_children == 0) return;

    log_msg("TURYSTA %d: OPIEKUN (wiek:%d, family:%d) z %d dziecmi",
            tourist_id, age, family_id, num_children);

    for (int i = 0; i < num_children; i++) {
        ChildData* data = malloc(sizeof(ChildData));
        if (!data) {
            perror("malloc ChildData");
            continue;
        }
        data->child_num = i + 1;
        data->age = rand() % (CHILD_NEEDS_GUARDIAN_MAX - AGE_MIN + 1) + AGE_MIN;

        if (pthread_create(&child_threads[i], NULL, child_thread_func, data) != 0) {
            perror("pthread_create child");
            free(data);
        }
    }
}

// Kupno biletu w kasie
void buy_ticket() {
    log_msg("TURYSTA %d: Podchodze do kasy (VIP:%d, family:%d).",
            tourist_id, is_vip, family_id);

    // Losowy czas dojscia do kasy (skrocony)
    usleep((rand() % 300000) + 100000);

    MsgBuf msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = MSG_TYPE_CASHIER_REQ;
    msg.pid = getpid();
    msg.tourist_id = tourist_id;
    msg.tourist_type = type;
    msg.age = age;
    msg.is_vip = is_vip;
    msg.num_children = num_children;
    msg.family_id = family_id;

    while (msgsnd(msg_queue_id, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
        if (errno == EINTR) continue;  // przerwane sygnalem - ponow
        if (errno == EIDRM || errno == EINVAL) return;  // kolejka usunieta
        perror("TURYSTA: msgsnd to cashier");
        return;
    }

    // Czekaj na bilet (polling - pozwala na przerwanie przez SIGTERM)
    while (tourist_running) {
        if (sem_trywait_op(sem_ticket_bought_id, tourist_id - 1) == 0) break;
        usleep(50000);
    }
    if (!tourist_running) return;

    // Sprawdz czy kasjer rzeczywiscie sprzedal bilet (mogl odrzucic)
    sem_wait_op(sem_state_mutex_id, 0);
    int got_ticket = tickets[tourist_id].is_valid;
    sem_signal_op(sem_state_mutex_id, 0);

    if (!got_ticket) {
        log_msg("TURYSTA %d: Kasa odrzucila - bramki zamkniete.", tourist_id);
        return;
    }
    log_msg("TURYSTA %d: Bilet kupiony (family:%d).", tourist_id, family_id);

    // Powiadom dzieci
    if (num_children > 0) {
        for (int i = 0; i < num_children; i++) {
            sem_signal_op(sem_ticket_bought_id, tourist_id - 1);
        }
        usleep(50000);
        log_msg("TURYSTA %d: Rodzina skompletowana, idziemy razem.", tourist_id);
    }
}

// Walidacja karnetu przy bramce wejsciowej
int validate_ticket_at_gate() {
    sem_wait_op(sem_state_mutex_id, 0);

    Ticket *t = &tickets[tourist_id];
    if (!t->is_valid) {
        sem_signal_op(sem_state_mutex_id, 0);
        log_msg("TURYSTA %d: Bilet nieaktywny! Odrzucony.", tourist_id);
        return 0;
    }

    // Sprawdz waznosc czasowa
    if (t->type == TK1 || t->type == TK2 || t->type == TK3) {
        time_t now = time(NULL);
        if (t->valid_until > 0 && now > t->valid_until) {
            sem_signal_op(sem_state_mutex_id, 0);
            log_msg("TURYSTA %d: Karnet czasowy WYGASL! Odrzucony.", tourist_id);
            stats->rejected_expired++;
            return 0;
        }
    }

    // Jednorazowy - sprawdz czy juz uzyty
    if (t->type == SINGLE && t->validation_count > 0) {
        sem_signal_op(sem_state_mutex_id, 0);
        log_msg("TURYSTA %d: Bilet jednorazowy juz uzyty! Odrzucony.", tourist_id);
        return 0;
    }

    // Zarejestruj przejscie
    t->validation_count++;

    if (state->pass_log_count < MAX_PASS_LOG) {
        PassLogEntry *entry = &state->pass_log[state->pass_log_count];
        entry->tourist_id = tourist_id;
        entry->ticket_type = t->type;
        entry->timestamp = time(NULL);
        entry->ride_number = t->validation_count;
        state->pass_log_count++;
    }

    sem_signal_op(sem_state_mutex_id, 0);

    log_msg("TURYSTA %d: Karnet zwalidowany (uzycie #%d).", tourist_id, t->validation_count);
    return 1;
}

// Wejscie przez bramki (4 bramki)
int enter_through_gates() {
    sem_wait_op(sem_state_mutex_id, 0);
    int gates_open = !state->is_closing;
    sem_signal_op(sem_state_mutex_id, 0);

    if (!gates_open) {
        log_msg("TURYSTA %d: Bramki zamkniete. Rezygnuje.", tourist_id);
        return 0;
    }

    int gate_num;
    if (is_vip) {
        // VIP wchodzi bez kolejki - wybiera najkrotsza bramke
        gate_num = rand() % ENTRY_GATES;
        log_msg("TURYSTA %d: VIP - priorytetowe wejscie (bramka %d).", tourist_id, gate_num + 1);
    } else {
        gate_num = rand() % ENTRY_GATES;
        log_msg("TURYSTA %d: Czekam na wejscie (bramka %d).", tourist_id, gate_num + 1);
    }

    // VIP nie czeka (trywait), zwykly czeka
    if (is_vip) {
        // VIP probuje bez czekania, jesli nie moze - czeka krotko
        if (sem_trywait_op(sem_entry_id, gate_num) != 0) {
            sem_wait_op(sem_entry_id, gate_num);
        }
    } else {
        sem_wait_op(sem_entry_id, gate_num);
    }

    // Walidacja biletu na bramce
    if (!validate_ticket_at_gate()) {
        sem_signal_op(sem_entry_id, gate_num);
        return 0;
    }

    log_msg("TURYSTA %d: Przeszedlem bramke %d (walidacja OK).", tourist_id, gate_num + 1);

    usleep((rand() % 100000) + 50000);
    sem_signal_op(sem_entry_id, gate_num);
    return 1;
}

// Wejscie na teren stacji dolnej (max N osob)
void enter_station() {
    int group_size = 1 + num_children;

    log_msg("TURYSTA %d: Czekam na miejsce na stacji (%d osob).", tourist_id, group_size);

    for (int i = 0; i < group_size; i++) {
        sem_wait_op(sem_station_id, 0);
    }

    sem_wait_op(sem_state_mutex_id, 0);
    state->station_population += group_size;
    int pop = state->station_population;
    sem_signal_op(sem_state_mutex_id, 0);

    log_msg("TURYSTA %d: Wszedlem na stacje. Populacja: %d/%d",
            tourist_id, pop, MAX_STATION_CAPACITY);
}

// Przejscie na peron (3 bramki peronowe - otwierane przez pracownika1 po kontroli grupy)
void go_to_platform() {
    int platform_gate = rand() % PLATFORM_GATES;

    log_msg("TURYSTA %d: Pracownik1 otworzyl bramke peronowa %d - przechodze na peron.",
            tourist_id, platform_gate + 1);

    sem_wait_op(sem_platform_id, platform_gate);

    log_msg("TURYSTA %d: Przeszedlem bramke peronowa %d. Wsiadam na krzeslo.",
            tourist_id, platform_gate + 1);

    usleep((rand() % 100000) + 30000);
    sem_signal_op(sem_platform_id, platform_gate);
}

// Oczekiwanie na krzeslo - wyslij zgloszenie do pracownika1
int wait_for_chair() {
    MsgBuf worker_msg;
    memset(&worker_msg, 0, sizeof(worker_msg));
    worker_msg.mtype = MSG_TYPE_WORKER_DOWN;
    worker_msg.pid = getpid();
    worker_msg.tourist_id = tourist_id;
    worker_msg.tourist_type = type;
    worker_msg.age = age;
    worker_msg.is_vip = is_vip;
    worker_msg.num_children = num_children;
    worker_msg.family_id = family_id;
    worker_msg.requires_guardian = (age < CHILD_NEEDS_GUARDIAN_MAX) ? 1 : 0;
    worker_msg.action = ACTION_TOURIST_READY;

    while (msgsnd(msg_queue_id, &worker_msg, sizeof(worker_msg) - sizeof(long), 0) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return 0;
        perror("TURYSTA: msgsnd to worker");
        return 0;
    }

    log_msg("TURYSTA %d: Zgloszenie do pracownika, czekam na przydział krzesla.", tourist_id);

    // Czekaj na odpowiedz od pracownika (polling - pozwala na przerwanie)
    MsgBuf go_msg;
    while (tourist_running) {
        ssize_t r = msgrcv(msg_queue_id, &go_msg, sizeof(go_msg) - sizeof(long),
                           getpid(), IPC_NOWAIT);
        if (r >= 0) break;
        if (errno == ENOMSG) { usleep(50000); continue; }
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return 0;
        perror("TURYSTA: msgrcv wait for chair");
        return 0;
    }
    if (!tourist_running) return 0;

    if (go_msg.action == ACTION_REJECTED) {
        log_msg("TURYSTA %d: Odrzucony przez pracownika.", tourist_id);
        return 0;
    }

    log_msg("TURYSTA %d: Pracownik1 przydzielil krzeslo - ide na peron.", tourist_id);
    return 1;
}

// Przejazd w gore
void ride_up() {
    sleep(RIDE_DURATION);
    log_msg("TURYSTA %d: Dojechalem na GORE. Wysiadam.", tourist_id);
}

// Wyjscie ze stacji gornej (2 drogi, obslugiwane przez pracownika2)
void exit_upper_station() {
    // Powiadom pracownika2 o przybyciu
    MsgBuf arrive_msg;
    memset(&arrive_msg, 0, sizeof(arrive_msg));
    arrive_msg.mtype = MSG_TYPE_WORKER_UP;
    arrive_msg.pid = getpid();
    arrive_msg.tourist_id = tourist_id;
    arrive_msg.action = ACTION_ARRIVED_TOP;

    while (msgsnd(msg_queue_id, &arrive_msg, sizeof(arrive_msg) - sizeof(long), 0) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        perror("TURYSTA: msgsnd to worker2");
        return;
    }

    // Czekaj na potwierdzenie wyjscia od pracownika2 (polling)
    MsgBuf exit_ack;
    while (tourist_running) {
        ssize_t r = msgrcv(msg_queue_id, &exit_ack, sizeof(exit_ack) - sizeof(long),
                           getpid(), IPC_NOWAIT);
        if (r >= 0) break;
        if (errno == ENOMSG) { usleep(50000); continue; }
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        perror("TURYSTA: msgrcv exit ack");
        return;
    }

    log_msg("TURYSTA %d: Opuscilem stacje gorna.", tourist_id);
}

// Zjazd trasa (rowerzysta) lub zejscie (pieszy)
void descend() {
    int route = rand() % 3;
    int duration;
    const char *route_name;

    if (route == 0) {
        duration = ROUTE_TIME_T1;
        route_name = "T1 (latwa)";
        sem_wait_op(sem_state_mutex_id, 0);
        stats->route_t1++;
        sem_signal_op(sem_state_mutex_id, 0);
    } else if (route == 1) {
        duration = ROUTE_TIME_T2;
        route_name = "T2 (srednia)";
        sem_wait_op(sem_state_mutex_id, 0);
        stats->route_t2++;
        sem_signal_op(sem_state_mutex_id, 0);
    } else {
        duration = ROUTE_TIME_T3;
        route_name = "T3 (trudna)";
        sem_wait_op(sem_state_mutex_id, 0);
        stats->route_t3++;
        sem_signal_op(sem_state_mutex_id, 0);
    }

    if (type == BIKER) {
        log_msg("TURYSTA %d: ROWERZYSTA zjezdza trasa %s (%ds).", tourist_id, route_name, duration);
    } else {
        log_msg("TURYSTA %d: PIESZY schodzi trasa %s (%ds).", tourist_id, route_name, duration);
    }

    sleep(duration);
    log_msg("TURYSTA %d: Zakonczylem zjazd/zejscie.", tourist_id);
}

// Opuszczenie systemu - zwolnij krzeslo i miejsce na stacji
void leave_system() {
    int group_size = 1 + num_children;

    // Zwolnij miejsce na stacji
    sem_wait_op(sem_state_mutex_id, 0);
    state->station_population -= group_size;
    sem_signal_op(sem_state_mutex_id, 0);

    for (int i = 0; i < group_size; i++) {
        sem_signal_op(sem_station_id, 0);
    }

    // Powiadom pracownika1 o zwolnieniu krzesla
    MsgBuf chair_free_msg;
    memset(&chair_free_msg, 0, sizeof(chair_free_msg));
    chair_free_msg.mtype = MSG_TYPE_WORKER_DOWN;
    chair_free_msg.action = ACTION_CHAIR_FREE;
    chair_free_msg.pid = getpid();
    chair_free_msg.tourist_id = tourist_id;
    while (msgsnd(msg_queue_id, &chair_free_msg, sizeof(chair_free_msg) - sizeof(long), 0) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) break;
        perror("TURYSTA: msgsnd chair_free");
        break;
    }

    log_msg("TURYSTA %d: Opuszczam system. Do zobaczenia!", tourist_id);
}

// Sprawdz czy bilet pozwala na kolejny przejazd
int can_ride_again() {
    sem_wait_op(sem_state_mutex_id, 0);
    Ticket *t = &tickets[tourist_id];

    // Bramki zamkniete - koniec
    if (state->is_closing) {
        sem_signal_op(sem_state_mutex_id, 0);
        return 0;
    }

    if (!t->is_valid) {
        sem_signal_op(sem_state_mutex_id, 0);
        return 0;
    }

    // Jednorazowy - tylko 1 przejazd
    if (t->type == SINGLE) {
        sem_signal_op(sem_state_mutex_id, 0);
        return 0;
    }

    // Karnety czasowe - sprawdz waznosc
    if (t->type == TK1 || t->type == TK2 || t->type == TK3) {
        time_t now = time(NULL);
        if (t->valid_until > 0 && now > t->valid_until) {
            sem_signal_op(sem_state_mutex_id, 0);
            log_msg("TURYSTA %d: Karnet czasowy wygasl, koncze przejazdy.", tourist_id);
            return 0;
        }
    }

    // DAILY lub wazny czasowy - mozna jechac ponownie
    sem_signal_op(sem_state_mutex_id, 0);
    return 1;
}

// Glowna procedura turysty
void run_tourist() {
    tourist_id = atoi(getenv("TOURIST_ID"));
    srand(time(NULL) ^ getpid());

    age = rand() % (AGE_MAX - AGE_MIN + 1) + AGE_MIN;
    type = (rand() % 2 == 0) ? WALKER : BIKER;
    is_vip = (rand() % 100 < VIP_PROBABILITY) ? 1 : 0;

    // Losowy czas przybycia (skalowany z liczba turystow)
    int max_delay = NUM_TOURISTS / 4;
    if (max_delay < 20) max_delay = 20;
    if (max_delay > CLOSING_TIME * 2 / 3) max_delay = CLOSING_TIME * 2 / 3;
    int arrival_delay = rand() % max_delay;
    if (arrival_delay > 0) {
        sleep(arrival_delay);
    }

    // Czy dorosly z dziecmi?
    if (age >= GUARDIAN_AGE_MIN && (rand() % 100 < 25)) {
        num_children = rand() % MAX_CHILDREN_PER_GUARDIAN + 1;
        sem_wait_op(sem_state_mutex_id, 0);
        family_id = state->next_family_id++;
        sem_signal_op(sem_state_mutex_id, 0);
        create_family_threads();
    }

    // Czy dziecko wymagajace opiekuna?
    int requires_guardian = (age < CHILD_NEEDS_GUARDIAN_MAX) ? 1 : 0;

    log_msg("TURYSTA %d: Start (wiek:%d, %s, VIP:%d, family:%d, dzieci:%d, wymaga_opiekuna:%d)",
            tourist_id, age, type == BIKER ? "Rowerzysta" : "Pieszy",
            is_vip, family_id, num_children, requires_guardian);

    // Dzieci <= 8 lat musza miec opiekuna - samotne dziecko nie moze korzystac z kolei
    if (age < CHILD_NEEDS_GUARDIAN_MAX && family_id == 0) {
        log_msg("TURYSTA %d: Dziecko (wiek:%d) bez opiekuna - nie moze korzystac z kolei.",
                tourist_id, age);
        usleep((rand() % 300000) + 100000);
        goto cleanup;
    }

    // Niektory turyści nie korzystaja z kolei (wg opisu)
    if (rand() % 100 < 10) {
        log_msg("TURYSTA %d: Nie korzystam z kolei, spaceruje.", tourist_id);
        usleep((rand() % 500000) + 200000);
        goto cleanup;
    }

    // 1. Kupno biletu w kasie
    buy_ticket();
    if (!tourist_running) goto cleanup;

    // Sprawdz czy bramki jeszcze otwarte
    sem_wait_op(sem_state_mutex_id, 0);
    int gates_open = !state->is_closing;
    sem_signal_op(sem_state_mutex_id, 0);
    if (!gates_open) {
        log_msg("TURYSTA %d: Kolej zamknieta po kupnie biletu.", tourist_id);
        goto cleanup;
    }

    // === PETLA WIELOKROTNYCH PRZEJAZDOW ===
    int ride_count = 0;
    while (tourist_running) {
        ride_count++;

        // 2. Przejscie przez bramki wejsciowe (walidacja biletu)
        if (!enter_through_gates()) {
            break;
        }
        if (!tourist_running) break;

        // 3. Wejscie na teren stacji dolnej
        enter_station();
        if (!tourist_running) goto leave_station;

        // 4. Oczekiwanie na krzeslo - zgloszenie do pracownika1
        //    (Pracownik1 kontroluje grupe i otwiera bramke peronowa)
        if (!wait_for_chair()) {
            goto leave_station;
        }
        if (!tourist_running) goto leave_station;

        // 5. Przejscie przez bramke peronowa (otwarta przez pracownika1)
        go_to_platform();

        // 6. Przejazd w gore
        ride_up();

        // 7. Wyjscie ze stacji gornej (2 drogi)
        exit_upper_station();

        // 8. Zjazd/zejscie trasa
        descend();

        // 9. Zwolnij krzeslo i miejsce na stacji
        leave_system();

        // Sprawdz czy mozna jechac ponownie
        if (!can_ride_again()) {
            log_msg("TURYSTA %d: Zakonczylem przejazdy (laczna liczba: %d).", tourist_id, ride_count);
            break;
        }

        log_msg("TURYSTA %d: Karnet wazny, ide na kolejny przejazd (#%d).", tourist_id, ride_count + 1);
        // Krotka przerwa przed kolejnym przejazdem
        usleep((rand() % 500000) + 200000);
    }

    goto cleanup;

leave_station:
    {
        int group_size = 1 + num_children;
        sem_wait_op(sem_state_mutex_id, 0);
        state->station_population -= group_size;
        sem_signal_op(sem_state_mutex_id, 0);
        for (int i = 0; i < group_size; i++) {
            sem_signal_op(sem_station_id, 0);
        }
        log_msg("TURYSTA %d: Opuszczam stacje (przerwane).", tourist_id);
    }

cleanup:
    // Czyszczenie watkow dzieci
    if (num_children > 0) {
        force_exit = 1;
        for (int i = 0; i < num_children; i++) {
            sem_signal_op(sem_ticket_bought_id, tourist_id - 1);
        }
        for (int i = 0; i < num_children; i++) {
            pthread_join(child_threads[i], NULL);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uzycie: %s <tourist_id>\n", argv[0]);
        exit(1);
    }

    // Walidacja danych wejsciowych
    int id = atoi(argv[1]);
    if (id <= 0 || id > NUM_TOURISTS) {
        fprintf(stderr, "BLAD: tourist_id musi byc w zakresie 1-%d, podano: %s\n",
                NUM_TOURISTS, argv[1]);
        exit(1);
    }

    setenv("TOURIST_ID", argv[1], 1);

    struct sigaction sa;
    sa.sa_handler = tourist_sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    attach_ipc_resources();
    run_tourist();

    return 0;
}
