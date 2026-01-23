#include "common.h"

volatile sig_atomic_t worker_is_running = 1;
volatile sig_atomic_t emergency_stop = 0;
int fifo_fd = -1;

void worker_sigterm_handler(int sig) {
    (void)sig;
    worker_is_running = 0;
}

// SIGUSR1 - zatrzymanie kolei w przypadku zagrozenia
void worker_sigusr1_handler(int sig) {
    (void)sig;
    emergency_stop = 1;
}

// SIGUSR2 - wznowienie kolei
void worker_sigusr2_handler(int sig) {
    (void)sig;
    emergency_stop = 0;
}

// === FUNKCJE KOLEJKI Z LAZY DELETION ===

int shared_q_is_empty(SharedQueue *q) {
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % MAX_Q_SIZE;
        if (!q->items[idx].is_removed) {
            return 0;
        }
    }
    return 1;
}

QueuedTourist shared_q_pop(SharedQueue *q) {
    while (q->count > 0) {
        QueuedTourist t = q->items[q->head];
        q->head = (q->head + 1) % MAX_Q_SIZE;
        q->count--;
        if (!t.is_removed) {
            return t;
        }
    }
    QueuedTourist empty = { .pid = -1, .is_removed = 1 };
    return empty;
}

void shared_q_mark_removed(SharedQueue *q, int logical_idx) {
    if (logical_idx < 0 || logical_idx >= q->count) return;
    int physical_idx = (q->head + logical_idx) % MAX_Q_SIZE;
    q->items[physical_idx].is_removed = 1;
}

void shared_q_compact(SharedQueue *q) {
    QueuedTourist temp[MAX_Q_SIZE];
    int new_count = 0;
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % MAX_Q_SIZE;
        if (!q->items[idx].is_removed) {
            temp[new_count++] = q->items[idx];
        }
    }
    q->head = 0;
    q->tail = new_count;
    q->count = new_count;
    for (int i = 0; i < new_count; i++) {
        q->items[i] = temp[i];
    }
}

// VIP - wstaw na poczatek kolejki (priorytet)
void shared_q_push_vip(SharedQueue *q, QueuedTourist t) {
    if (q->count >= MAX_Q_SIZE) return;
    // Przesun head do tylu i wstaw na poczatek
    q->head = (q->head - 1 + MAX_Q_SIZE) % MAX_Q_SIZE;
    q->items[q->head] = t;
    q->count++;
}

void shared_q_push_back(SharedQueue *q, QueuedTourist t) {
    if (q->count >= MAX_Q_SIZE) return;
    q->items[q->tail] = t;
    q->tail = (q->tail + 1) % MAX_Q_SIZE;
    q->count++;
}

// === WYSLIJ TURYSTOM SYGNAL ODJAZDU ===
void send_tourists_go(QueuedTourist group[], int size) {
    for (int i = 0; i < size; i++) {
        MsgBuf go_msg;
        memset(&go_msg, 0, sizeof(go_msg));
        go_msg.mtype = group[i].pid;
        go_msg.action = ACTION_TOURIST_READY;
        if (msgsnd(msg_queue_id, &go_msg, sizeof(go_msg) - sizeof(long), 0) == -1) {
            perror("msgsnd send_tourists_go");
        }
    }
}

// === ZATRZYMANIE/WZNOWIENIE KOLEI (FIFO) ===
void handle_emergency_stop() {
    log_msg("PRACOWNIK1: ZAGROZENIE! Zatrzymuje kolej (SIGUSR1).");

    sem_wait_op(sem_state_mutex_id, 0);
    state->is_paused = 1;
    sem_signal_op(sem_state_mutex_id, 0);

    // Wyslij zadanie pauzy do pracownika2 przez FIFO
    if (fifo_fd >= 0) {
        FifoMsg fmsg;
        fmsg.type = FIFO_PAUSE_REQ;
        fmsg.sender_pid = getpid();
        if (write(fifo_fd, &fmsg, sizeof(fmsg)) == -1) {
            perror("write FIFO_PAUSE_REQ");
        }
        log_msg("PRACOWNIK1: Wyslano PAUSE_REQ do pracownika2 przez FIFO.");
    }

    // Czekaj na potwierdzenie od pracownika2
    log_msg("PRACOWNIK1: Czekam na potwierdzenie od pracownika2...");
    if (fifo_fd >= 0) {
        FifoMsg ack;
        ssize_t r = read(fifo_fd, &ack, sizeof(ack));
        if (r > 0 && ack.type == FIFO_PAUSE_ACK) {
            log_msg("PRACOWNIK1: Otrzymano PAUSE_ACK od pracownika2.");
        }
    }

    log_msg("PRACOWNIK1: Kolej ZATRZYMANA. Oczekiwanie na sygnal wznowienia (SIGUSR2)...");

    // Czekaj az emergency_stop zostanie wyzerowane przez SIGUSR2
    while (emergency_stop && worker_is_running) {
        usleep(100000);
    }

    if (worker_is_running) {
        log_msg("PRACOWNIK1: Otrzymano SIGUSR2 - wznawiam kolej.");

        // Wyslij wznowienie do pracownika2
        if (fifo_fd >= 0) {
            FifoMsg fmsg;
            fmsg.type = FIFO_RESUME_REQ;
            fmsg.sender_pid = getpid();
            if (write(fifo_fd, &fmsg, sizeof(fmsg)) == -1) {
                perror("write FIFO_RESUME_REQ");
            }
        }

        sem_wait_op(sem_state_mutex_id, 0);
        state->is_paused = 0;
        sem_signal_op(sem_state_mutex_id, 0);

        log_msg("PRACOWNIK1: Kolej WZNOWIONA.");
    }
}

// === LOGIKA GRUPOWANIA NA KRZESELKO ===
// Zasady wg opisu:
// - max 2 rowerzystow
// - 1 rowerzysta + max 2 pieszych
// - max 4 pieszych
// - dziecko 4-8 lat z opiekunem (dorosly max 2 dzieci)

void try_to_dispatch_groups(SharedQueue *queue) {
    static int operation_counter = 0;

    sem_wait_op(sem_state_mutex_id, 0);

    if (++operation_counter > 50) {
        shared_q_compact(queue);
        operation_counter = 0;
    }

    // Sprawdz czy kolej nie jest wstrzymana
    if (state->is_paused) {
        sem_signal_op(sem_state_mutex_id, 0);
        return;
    }

    int dispatched_in_pass;

    do {
        dispatched_in_pass = 0;

        if (state->busy_chairs >= MAX_BUSY_CHAIRS) {
            break;
        }

        if (shared_q_is_empty(queue)) {
            break;
        }

        // === PROBA 1: GRUPA RODZINNA (opiekun + dzieci jako watki) ===
        // Dzieci sa watkami w procesie opiekuna - nie sa oddzielnymi wpisami w kolejce.
        // Opiekun ma num_children > 0, zajmuje 1+num_children miejsc na krzesle.
        int family_idx = -1;
        for (int i = 0; i < queue->count; i++) {
            int idx = (queue->head + i) % MAX_Q_SIZE;
            if (!queue->items[idx].is_removed &&
                queue->items[idx].num_children > 0) {
                family_idx = i;
                break;
            }
        }

        if (family_idx >= 0) {
            int f_phys = (queue->head + family_idx) % MAX_Q_SIZE;
            QueuedTourist guardian = queue->items[f_phys];
            int family_size = 1 + guardian.num_children; // opiekun + dzieci

            if (family_size <= CHAIR_CAPACITY) {
                QueuedTourist group[CHAIR_CAPACITY];
                group[0] = guardian;
                // Dzieci sa watkami - nie trzeba ich z kolejki pobierac
                // Tylko opiekun jest w kolejce, ale zajmuja family_size miejsc

                // Mozna dolaczyc pieszych do wypelnienia (max 4 miejsc)
                int remaining = CHAIR_CAPACITY - family_size;
                int extra_indices[CHAIR_CAPACITY];
                int extra_count = 0;
                for (int i = 0; i < queue->count && extra_count < remaining; i++) {
                    int idx = (queue->head + i) % MAX_Q_SIZE;
                    if (i != family_idx &&
                        !queue->items[idx].is_removed &&
                        queue->items[idx].num_children == 0 &&
                        queue->items[idx].type == WALKER) {
                        extra_indices[extra_count] = i;
                        group[family_size + extra_count] = queue->items[idx];
                        extra_count++;
                    }
                }

                shared_q_mark_removed(queue, family_idx);
                for (int i = 0; i < extra_count; i++) {
                    shared_q_mark_removed(queue, extra_indices[i]);
                }

                // Wysylamy sygnal tylko opiekunowi i dodatkowym osobom
                // (dzieci sa watkami opiekuna, pojada razem z nim)
                int msg_count = 1 + extra_count;
                QueuedTourist msg_group[CHAIR_CAPACITY];
                msg_group[0] = guardian;
                for (int i = 0; i < extra_count; i++) {
                    msg_group[1 + i] = group[family_size + i];
                }
                send_tourists_go(msg_group, msg_count);

                state->busy_chairs++;   // 1 krzeslo zajete
                stats->total_rides++;
                stats->total_people += family_size + extra_count;
                // Opiekun: rowerzysta lub pieszy
                if (guardian.type == BIKER) stats->bikers++;
                else stats->walkers++;
                // Dzieci liczone jako piesi z opiekunem
                stats->walkers += guardian.num_children;
                stats->with_guardian += guardian.num_children;
                // Dodatkowi piesi
                stats->walkers += extra_count;
                dispatched_in_pass = 1;

                log_msg("PRACOWNIK1: RODZINA (opiekun+%d dzieci, +%d dod.) -> krzeslo %d/%d",
                    guardian.num_children, extra_count, state->busy_chairs, MAX_BUSY_CHAIRS);
                continue;
            }
        }

        // === PROBA 2: GRUPA ROWERZYSTOW (max 2 na krzeslo) ===
        {
            int biker_indices[MAX_BIKERS_PER_CHAIR];
            int biker_count = 0;
            for (int i = 0; i < queue->count && biker_count < MAX_BIKERS_PER_CHAIR; i++) {
                int idx = (queue->head + i) % MAX_Q_SIZE;
                if (!queue->items[idx].is_removed &&
                    queue->items[idx].num_children == 0 &&
                    queue->items[idx].type == BIKER) {
                    biker_indices[biker_count++] = i;
                }
            }

            if (biker_count == MAX_BIKERS_PER_CHAIR) {
                QueuedTourist group[MAX_BIKERS_PER_CHAIR];
                for (int i = 0; i < biker_count; i++) {
                    int phys = (queue->head + biker_indices[i]) % MAX_Q_SIZE;
                    group[i] = queue->items[phys];
                    shared_q_mark_removed(queue, biker_indices[i]);
                }
                send_tourists_go(group, biker_count);
                state->busy_chairs++;
                stats->total_rides++;
                stats->total_people += biker_count;
                stats->bikers += biker_count;
                dispatched_in_pass = 1;

                log_msg("PRACOWNIK1: 2 ROWERZ. -> krzeslo %d/%d",
                    state->busy_chairs, MAX_BUSY_CHAIRS);
                continue;
            }

            // 1 rowerzysta + max 2 pieszych
            if (biker_count >= 1) {
                int walker_indices[2];
                int walker_count = 0;
                for (int i = 0; i < queue->count && walker_count < 2; i++) {
                    int idx = (queue->head + i) % MAX_Q_SIZE;
                    if (!queue->items[idx].is_removed &&
                        queue->items[idx].num_children == 0 &&
                        queue->items[idx].type == WALKER &&
                        i != biker_indices[0]) {
                        walker_indices[walker_count++] = i;
                    }
                }

                if (walker_count >= 1) {
                    int total = 1 + walker_count;
                    QueuedTourist group[3];
                    int phys_b = (queue->head + biker_indices[0]) % MAX_Q_SIZE;
                    group[0] = queue->items[phys_b];
                    shared_q_mark_removed(queue, biker_indices[0]);

                    for (int i = 0; i < walker_count; i++) {
                        int phys_w = (queue->head + walker_indices[i]) % MAX_Q_SIZE;
                        group[1 + i] = queue->items[phys_w];
                        shared_q_mark_removed(queue, walker_indices[i]);
                    }

                    send_tourists_go(group, total);
                    state->busy_chairs++;
                    stats->total_rides++;
                    stats->total_people += total;
                    stats->bikers += 1;
                    stats->walkers += walker_count;
                    dispatched_in_pass = 1;

                    log_msg("PRACOWNIK1: 1 ROWERZ.+%d PIESZYCH -> krzeslo %d/%d",
                        walker_count, state->busy_chairs, MAX_BUSY_CHAIRS);
                    continue;
                }
            }
        }

        // === PROBA 3: GRUPA PIESZYCH (max 4 na krzeslo) ===
        {
            int walker_indices[CHAIR_CAPACITY];
            int walker_count = 0;
            for (int i = 0; i < queue->count && walker_count < CHAIR_CAPACITY; i++) {
                int idx = (queue->head + i) % MAX_Q_SIZE;
                if (!queue->items[idx].is_removed &&
                    queue->items[idx].num_children == 0 &&
                    queue->items[idx].type == WALKER) {
                    walker_indices[walker_count++] = i;
                }
            }

            if (walker_count >= 2) {
                // Wysylamy tylu pieszych ile jest (2-4)
                QueuedTourist group[CHAIR_CAPACITY];
                for (int i = 0; i < walker_count; i++) {
                    int phys = (queue->head + walker_indices[i]) % MAX_Q_SIZE;
                    group[i] = queue->items[phys];
                    shared_q_mark_removed(queue, walker_indices[i]);
                }

                send_tourists_go(group, walker_count);
                state->busy_chairs++;
                stats->total_rides++;
                stats->total_people += walker_count;
                stats->walkers += walker_count;
                dispatched_in_pass = 1;

                log_msg("PRACOWNIK1: %d PIESZYCH -> krzeslo %d/%d",
                    walker_count, state->busy_chairs, MAX_BUSY_CHAIRS);
                continue;
            }
        }

        // === PROBA 4: POJEDYNCZY (przy zamykaniu - wysylaj WSZYSTKICH) ===
        if (!state->is_running || state->is_closing) {
            for (int i = 0; i < queue->count; i++) {
                int idx = (queue->head + i) % MAX_Q_SIZE;
                if (!queue->items[idx].is_removed) {
                    QueuedTourist entry = queue->items[idx];
                    shared_q_mark_removed(queue, i);

                    QueuedTourist group[1];
                    group[0] = entry;
                    send_tourists_go(group, 1);
                    state->busy_chairs++;
                    stats->total_rides++;

                    int people = 1 + entry.num_children;
                    stats->total_people += people;
                    if (entry.type == BIKER) stats->bikers++;
                    else stats->walkers++;
                    // Jesli opiekun z dziecmi - policz dzieci
                    if (entry.num_children > 0) {
                        stats->walkers += entry.num_children;
                        stats->with_guardian += entry.num_children;
                    }
                    dispatched_in_pass = 1;

                    log_msg("PRACOWNIK1: POJEDYNCZY (zamykanie, osoby:%d) -> krzeslo %d/%d",
                        people, state->busy_chairs, MAX_BUSY_CHAIRS);
                    break;
                }
            }
        }

    } while (dispatched_in_pass && state->busy_chairs < MAX_BUSY_CHAIRS);

    sem_signal_op(sem_state_mutex_id, 0);
}

// === GLOWNA PETLA PRACOWNIKA1 ===
void run_worker_down() {
    log_msg("PRACOWNIK1: Proces pracownika stacji dolnej uruchomiony (PID:%d).", getpid());

    sem_wait_op(sem_state_mutex_id, 0);
    state->worker_down_pid = getpid();
    sem_signal_op(sem_state_mutex_id, 0);

    // Otworz FIFO do komunikacji z pracownikiem2
    fifo_fd = open(FIFO_WORKER_PATH, O_RDWR | O_NONBLOCK);
    if (fifo_fd == -1) {
        perror("PRACOWNIK1: open FIFO");
    }

    SharedQueue *queue = &state->waiting_tourists_down;
    MsgBuf msg;

    while (state->is_running && worker_is_running) {
        // Sprawdz awaryjne zatrzymanie
        if (emergency_stop) {
            handle_emergency_stop();
            if (!worker_is_running) break;
        }

        ssize_t result = msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long),
                                MSG_TYPE_WORKER_DOWN, IPC_NOWAIT);

        if (result == -1) {
            if (errno == ENOMSG) {
                // Brak wiadomosci - sprobuj wyslac grupy z kolejki
                try_to_dispatch_groups(queue);
                usleep(50000);
                continue;
            }
            if (!worker_is_running || errno == EIDRM || errno == EINVAL) break;
            continue;
        }

        if (!state->is_running) break;

        switch (msg.action) {
            case ACTION_TOURIST_READY: {
                sem_wait_op(sem_state_mutex_id, 0);

                if (queue->count >= MAX_Q_SIZE) {
                    log_msg("PRACOWNIK1: Kolejka pelna! Turysta %d odrzucony.", msg.tourist_id);
                    sem_signal_op(sem_state_mutex_id, 0);

                    MsgBuf reject_msg;
                    memset(&reject_msg, 0, sizeof(reject_msg));
                    reject_msg.mtype = msg.pid;
                    reject_msg.action = ACTION_REJECTED;
                    msgsnd(msg_queue_id, &reject_msg, sizeof(reject_msg) - sizeof(long), 0);
                    break;
                }

                QueuedTourist new_tourist = {
                    .pid = msg.pid,
                    .type = msg.tourist_type,
                    .age = msg.age,
                    .requires_guardian = msg.requires_guardian,
                    .is_removed = 0,
                    .num_children = msg.num_children,
                    .tourist_id = msg.tourist_id,
                    .family_id = msg.family_id,
                    .is_vip = msg.is_vip
                };

                // VIP - priorytet w kolejce
                if (msg.is_vip) {
                    shared_q_push_vip(queue, new_tourist);
                    stats->vip_served++;
                    log_msg("PRACOWNIK1: VIP turysta %d dodany na poczatek kolejki!", msg.tourist_id);
                } else {
                    shared_q_push_back(queue, new_tourist);
                }

                sem_signal_op(sem_state_mutex_id, 0);
                break;
            }
            case ACTION_CHAIR_FREE:
                sem_wait_op(sem_state_mutex_id, 0);
                if (state->busy_chairs > 0) {
                    state->busy_chairs--;
                }
                sem_signal_op(sem_state_mutex_id, 0);
                break;
            default:
                break;
        }

        try_to_dispatch_groups(queue);
    }

    // Finalne oproznianie kolejki
    log_msg("PRACOWNIK1: Finalne oproznianie kolejki...");
    int final_attempts = 0;
    while (!shared_q_is_empty(queue) && final_attempts < 20) {
        try_to_dispatch_groups(queue);
        usleep(100000);
        final_attempts++;
    }

    sem_wait_op(sem_state_mutex_id, 0);
    shared_q_compact(queue);
    sem_signal_op(sem_state_mutex_id, 0);

    if (fifo_fd >= 0) close(fifo_fd);
    log_msg("PRACOWNIK1: Proces pracownika zakonczony.");
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    struct sigaction sa_term, sa_usr1, sa_usr2;

    sa_term.sa_handler = worker_sigterm_handler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, NULL);

    sa_usr1.sa_handler = worker_sigusr1_handler;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = 0;
    sigaction(SIGUSR1, &sa_usr1, NULL);

    sa_usr2.sa_handler = worker_sigusr2_handler;
    sigemptyset(&sa_usr2.sa_mask);
    sa_usr2.sa_flags = 0;
    sigaction(SIGUSR2, &sa_usr2, NULL);

    signal(SIGALRM, SIG_IGN);
    srand(time(NULL) ^ getpid());
    attach_ipc_resources();

    run_worker_down();

    return 0;
}
