#include "common.h"

volatile sig_atomic_t cashier_running = 1;

// Obliczanie ceny biletu z uwzglednieniem znizek
// Dzieci < 10 lat i seniorzy >= 65 lat maja 25% znizki
float calculate_ticket_price(TicketType type, int age) {
    float base_price = 0;
    switch (type) {
        case SINGLE: base_price = 10.0f; break;
        case TK1:    base_price = 30.0f; break;
        case TK2:    base_price = 50.0f; break;
        case TK3:    base_price = 70.0f; break;
        case DAILY:  base_price = 100.0f; break;
    }

    // 25% znizki dla dzieci < 10 lat i seniorow >= 65 lat
    if (age < CHILD_DISCOUNT_MAX || age >= SENIOR_AGE_LIMIT) {
        return base_price * 0.75f;
    }

    return base_price;
}

const char* ticket_type_name(TicketType type) {
    switch (type) {
        case SINGLE: return "JEDNORAZOWY";
        case TK1:    return "CZASOWY_1H";
        case TK2:    return "CZASOWY_2H";
        case TK3:    return "CZASOWY_3H";
        case DAILY:  return "DZIENNY";
        default:     return "NIEZNANY";
    }
}

void cashier_sigterm_handler(int sig) {
    (void)sig;
    cashier_running = 0;
}

int main(void) {
    process_role = ROLE_CASHIER;

    struct sigaction sa;
    sa.sa_handler = cashier_sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGINT, SIG_IGN);  // SIGINT obsluguje main, kasjer czeka na SIGTERM

    srand(time(NULL) ^ getpid());
    attach_ipc_resources();

    log_msg("KASJER: Proces kasjera uruchomiony (PID:%d).", getpid());

    MsgBuf msg;
    while (state->is_running && cashier_running) {
        ssize_t result = msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long),
                                MSG_TYPE_CASHIER_REQ, IPC_NOWAIT);

        if (result == -1) {
            if (errno == ENOMSG) {
                usleep(50000);
                continue;
            }
            if (errno == EIDRM || errno == EINVAL) break;
            if (errno != EINTR) perror("KASJER: msgrcv");
            continue;
        }

        if (!state->is_running || !cashier_running) break;

        // Sprawdz czy bramki sa zamkniete
        sem_wait_op(sem_state_mutex_id, 0);
        int gates_closed = state->is_closing;
        sem_signal_op(sem_state_mutex_id, 0);

        if (gates_closed) {
            log_msg("KASJER: Bramki zamkniete - odrzucam turystę %d.", msg.tourist_id);
            // Odblokuj turystę zeby nie czekal w nieskonczonosc
            sem_signal_op(sem_ticket_bought_id, msg.tourist_id - 1);
            continue;
        }

        // Czas obslugi (symulacja) - skalowany z liczba turystow
        {
            int svc_max = 200000 * 50 / NUM_TOURISTS;
            int svc_min = 100000 * 50 / NUM_TOURISTS;
            if (svc_max < 10000) svc_max = 10000;
            if (svc_min < 5000) svc_min = 5000;
            usleep((rand() % svc_max) + svc_min);
        }

        // Sprawdz ponownie po obsludze
        sem_wait_op(sem_state_mutex_id, 0);
        gates_closed = state->is_closing;
        if (gates_closed) {
            sem_signal_op(sem_state_mutex_id, 0);
            log_msg("KASJER: Bramki zamknely sie podczas obslugi - turysta %d.", msg.tourist_id);
            sem_signal_op(sem_ticket_bought_id, msg.tourist_id - 1);
            continue;
        }

        // Wybierz typ biletu (losowo, ale sensownie)
        int tourist_idx = msg.tourist_id;
        if (tourist_idx < 1 || tourist_idx > NUM_TOURISTS) {
            sem_signal_op(sem_state_mutex_id, 0);
            log_msg("KASJER: BLAD - nieprawidlowy tourist_id: %d", tourist_idx);
            continue;
        }

        TicketType chosen_type = (TicketType)(rand() % 5);
        tickets[tourist_idx].owner_pid = msg.pid;
        tickets[tourist_idx].type = chosen_type;
        tickets[tourist_idx].validation_count = 0;
        tickets[tourist_idx].family_id = msg.family_id;
        tickets[tourist_idx].is_valid = 1;

        // Waznosc karnetu czasowego
        time_t now = time(NULL);
        switch (chosen_type) {
            case TK1: tickets[tourist_idx].valid_until = now + 3600; break;
            case TK2: tickets[tourist_idx].valid_until = now + 7200; break;
            case TK3: tickets[tourist_idx].valid_until = now + 10800; break;
            case SINGLE:
            case DAILY:
            default: tickets[tourist_idx].valid_until = 0; break;  // bez limitu
        }

        // Statystyki sprzedazy
        switch (chosen_type) {
            case SINGLE: stats->single_sold++; break;
            case TK1:    stats->tk1_sold++; break;
            case TK2:    stats->tk2_sold++; break;
            case TK3:    stats->tk3_sold++; break;
            case DAILY:  stats->daily_sold++; break;
        }

        sem_signal_op(sem_state_mutex_id, 0);

        float price = calculate_ticket_price(chosen_type, msg.age);
        int has_discount = (msg.age < CHILD_DISCOUNT_MAX || msg.age >= SENIOR_AGE_LIMIT);

        log_msg("KASJER: Sprzedano %s turyscie %d (wiek:%d%s) za %.2f PLN",
                ticket_type_name(chosen_type), tourist_idx, msg.age,
                has_discount ? ", ZNIZKA 25%" : "", price);

        // Sygnalizuj turyscie ze bilet gotowy
        sem_signal_op(sem_ticket_bought_id, tourist_idx - 1);
    }

    log_msg("KASJER: Koniec pracy, zamykanie.");
    return 0;
}
