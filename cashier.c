#include "common.h"

float calculate_ticket_price(TicketType type, int age) {
    float base_price = 0;
    switch (type) {
        case SINGLE: base_price = 10.0; break;
        case TK1:    base_price = 30.0; break;
        case TK2:    base_price = 50.0; break;
        case TK3:    base_price = 70.0; break;
        case DAILY:  base_price = 100.0; break;
    }

    if (age < CHILD_AGE_LIMIT || age >= SENIOR_AGE_LIMIT) {
        return base_price * 0.75f;
    }
    return base_price;
}

void cashier_sigterm_handler(int sig) {
    log_msg("KASJER: Otrzymano SIGTERM, zamykanie.");
    exit(0);
}

int main(void) {
    signal(SIGTERM, cashier_sigterm_handler);
    srand(time(NULL) ^ getpid()); 

    attach_ipc_resources();
    log_msg("KASJER: Proces kasjera uruchomiony.");

    MsgBuf msg;
    while (state->is_running) {
        if (msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long), MSG_TYPE_CASHIER_REQ, 0) == -1) {
            if (errno == EIDRM || errno == EINVAL) break;
            perror("KASJER: msgrcv failed");
            continue;
        }

        if (!state->is_running) break;

        log_msg("KASJER: Przetwarzanie biletu dla turysty %d (wiek: %d)", msg.tourist_id, msg.age);

        int tourist_idx = msg.tourist_id;
        
        sem_wait(sem_state_mutex_id, 0);

        tickets[tourist_idx].owner_pid = msg.pid;
        tickets[tourist_idx].type = (TicketType)(rand() % 5);
        tickets[tourist_idx].validation_count = 0;
        
        //logika ważności biletów czasowych
        time_t now = time(NULL);
        switch (tickets[tourist_idx].type) {
            case TK1: tickets[tourist_idx].valid_until = now + 3600; break;
            case TK2: tickets[tourist_idx].valid_until = now + 7200; break;
            case TK3: tickets[tourist_idx].valid_until = now + 10800; break;
            default:  tickets[tourist_idx].valid_until = -1;
        }

        switch (tickets[tourist_idx].type) {
            case SINGLE: stats->single_sold++; break;
            case TK1:    stats->tk1_sold++; break;
            case TK2:    stats->tk2_sold++; break;
            case TK3:    stats->tk3_sold++; break;
            case DAILY:  stats->daily_sold++; break;
        }

        sem_signal(sem_state_mutex_id, 0);

        float price = calculate_ticket_price(tickets[tourist_idx].type, msg.age);
        log_msg("KASJER: Sprzedano bilet (%d) dla turysty %d za %.2f PLN", tickets[tourist_idx].type, msg.tourist_id, price);

        sem_signal(sem_ticket_bought_id, tourist_idx - 1);
    }

    log_msg("KASJER: Koniec pętli, zamykanie.");
    return 0;
}
