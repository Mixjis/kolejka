#include "common.h"

volatile sig_atomic_t worker_is_running = 1;
volatile sig_atomic_t is_paused = 0;

void worker_sigterm_handler(int sig) {
    worker_is_running = 0;
}

void worker_sigusr1_handler(int sig) {
    // SIGUSR1: zatrzymanie kolei
    is_paused = 1;
}

void worker_sigusr2_handler(int sig) {
    // SIGUSR2: wznowienie pracy
    is_paused = 0;
}

void send_tourists_go(QueuedTourist group[], int size) {
    log_msg("PRACOWNIK: Formowanie grupy %d-osobowej.", size);
    for (int i = 0; i < size; i++) {
        MsgBuf go_msg;
        go_msg.mtype = group[i].pid;
        if (msgsnd(msg_queue_id, &go_msg, sizeof(go_msg) - sizeof(long), 0) == -1) {
            if (errno != EIDRM) perror("Worker msgsnd to tourist");
        }
    }
}

int shared_q_is_empty(SharedQueue *q) {
    return q->count == 0;
}

QueuedTourist shared_q_pop(SharedQueue *q) {
    if (q->count <= 0) {
        QueuedTourist empty = { -1, -1 };
        return empty;
    }
    QueuedTourist t = q->items[q->head];
    q->head = (q->head + 1) % MAX_Q_SIZE;
    q->count--;
    return t;
}


void shared_q_remove_at(SharedQueue *q, int logical_idx) {
    if (logical_idx < 0 || logical_idx >= q->count) {
        return;
    }
    
 
    for (int i = logical_idx; i < q->count - 1; i++) {
        int curr_physical_idx = (q->head + i) % MAX_Q_SIZE;
        int next_physical_idx = (q->head + i + 1) % MAX_Q_SIZE;
        q->items[curr_physical_idx] = q->items[next_physical_idx];
    }
    q->count--;

    q->tail = (q->tail - 1 + MAX_Q_SIZE) % MAX_Q_SIZE;
}

void try_to_dispatch_groups(SharedQueue *queue, const char* log_prefix) {
    sem_wait(sem_state_mutex_id, 0);

    int dispatched_in_pass;
    do {
        dispatched_in_pass = 0;

        if (state->busy_chairs >= MAX_BUSY_CHAIRS) {
            break;
        }

        //GRUPY RODZINNE
        // Szukaj opiekuna (dorosłego) i zbierz z nim maksymalnie 2 dzieci
        int guardian_idx = -1;
        for (int i = 0; i < queue->count; i++) {
            int idx = (queue->head + i) % MAX_Q_SIZE;
            if (!queue->items[idx].requires_guardian && 
                queue->items[idx].age >= GUARDIAN_AGE_MIN) {
                guardian_idx = i;
                break;
            }
        }

        if (guardian_idx >= 0) {
            // Zbierz dzieci do tego opiekuna (max 2)
            int children_indices[2] = {-1, -1};
            int children_count = 0;
            
            for (int i = 0; i < queue->count && children_count < 2; i++) {
                int idx = (queue->head + i) % MAX_Q_SIZE;
                if (i != guardian_idx && queue->items[idx].requires_guardian) {
                    children_indices[children_count] = i;
                    children_count++;
                }
            }

            // Wysyłanie jeśli mamy przynajmniej 1 dziecko
            if (children_count > 0) {
                int total_seats_needed = 1 + children_count; // opiekun + dzieci
                if (state->busy_chairs + total_seats_needed <= MAX_BUSY_CHAIRS) {
                    
                    QueuedTourist guardian = queue->items[(queue->head + guardian_idx) % MAX_Q_SIZE];
                    QueuedTourist group[3]; // Max: opiekun + 2 dzieci
                    group[0] = guardian;
                    int group_size = 1;

                    // Dodawanie dzieci do grupy
                    for (int i = 0; i < children_count; i++) {
                        QueuedTourist child = queue->items[(queue->head + children_indices[i]) % MAX_Q_SIZE];
                        child.guardian_id = guardian.pid;
                        group[group_size] = child;
                        group_size++;
                    }
                    for (int i = children_count - 1; i >= 0; i--) {
                        shared_q_remove_at(queue, children_indices[i]);
                    }
                    // Opiekun może zmienić indeks po usunięciu dzieci, przeindeksuj
                    int adjusted_guardian_idx = guardian_idx;
                    for (int i = 0; i < children_count; i++) {
                        if (children_indices[i] < guardian_idx) {
                            adjusted_guardian_idx--;
                        }
                    }
                    shared_q_remove_at(queue, adjusted_guardian_idx);

                    // Wyślij grupę
                    send_tourists_go(group, group_size);
                    stats->walkers += group_size;
                    int new_busy = state->busy_chairs + group_size;
                    if (new_busy <= MAX_BUSY_CHAIRS) {
                        state->busy_chairs = new_busy;
                    } else {
                        log_msg("%s: BŁĄD! busy_chairs został by %d > %d. Nie wysyłam grupy!", 
                            log_prefix, new_busy, MAX_BUSY_CHAIRS);
                        break; // Wyjdź z pętli jeśli zabraknie miejsca
                    }
                    stats->total_rides++;
                    stats->with_guardian += children_count;
                    dispatched_in_pass = 1;
                    
                    log_msg("%s: GRUPA RODZINNA (opiekun PID:%d + %d dziecko/dzieci) wysłana! Zajęte: %d/%d.", 
                        log_prefix, guardian.pid, children_count, state->busy_chairs, MAX_BUSY_CHAIRS);
                    continue;
                }
            }
        }

        //GRUPY ZWYKŁE
        int walkers = 0, bikers = 0;
        for (int i = 0; i < queue->count; i++) {
            int idx = (queue->head + i) % MAX_Q_SIZE;
            if (!queue->items[idx].requires_guardian) {
                if (queue->items[idx].type == WALKER) walkers++;
                else bikers++;
            }
        }

        int group_size = 0;
        if (walkers >= 4) group_size = 4;
        else if (bikers >= 1 && walkers >= 2) group_size = 3;
        else if (bikers >= 2) group_size = 2;
        else if (walkers >= 3) group_size = 3;
        else if (bikers >= 1 && walkers >= 1) group_size = 2;
        else if (walkers >= 2) group_size = 2;
        else if (bikers >= 1) group_size = 1;
        else if (walkers >= 1) group_size = 1;

        if (group_size > 0 && state->busy_chairs + group_size <= MAX_BUSY_CHAIRS) {
            QueuedTourist group[4];
            int indices_to_remove[4];
            int selected_count = 0;

            int walkers_in_group = 0;
            int bikers_in_group = 0;

            for (int i = 0; i < queue->count && selected_count < group_size; i++) {
                int idx = (queue->head + i) % MAX_Q_SIZE;
                QueuedTourist candidate = queue->items[idx];
                
                if (!candidate.requires_guardian) {
                    group[selected_count] = candidate;
                    indices_to_remove[selected_count] = i;
                    selected_count++;
                    
                    if (candidate.type == WALKER) walkers_in_group++;
                    else bikers_in_group++;
                }
            }

            if (selected_count == group_size) {
                for (int i = selected_count - 1; i >= 0; i--) {
                    shared_q_remove_at(queue, indices_to_remove[i]);
                }

                send_tourists_go(group, selected_count);
                stats->bikers += bikers_in_group;
                stats->walkers += walkers_in_group;
                int new_busy = state->busy_chairs + selected_count;
                if (new_busy <= MAX_BUSY_CHAIRS) {
                    state->busy_chairs = new_busy;
                } else {
                    log_msg("%s: BŁĄD! busy_chairs był by %d > %d. Nie wysyłam grupy!", 
                        log_prefix, new_busy, MAX_BUSY_CHAIRS);
                    break;
                }
                stats->total_rides++;
                dispatched_in_pass = 1;
                
                log_msg("%s: Grupa (%d os.) odjechała. Zajęte miejsca: %d/%d. Kolejka: %d osób.", 
                    log_prefix, selected_count, state->busy_chairs, MAX_BUSY_CHAIRS, queue->count);
            }
        }

    } while (dispatched_in_pass && state->busy_chairs < MAX_BUSY_CHAIRS);

    sem_signal(sem_state_mutex_id, 0);
}

void run_worker(long msg_type, const char* log_prefix) {
    log_msg("%s: Proces pracownika uruchomiony.", log_prefix);

    SharedQueue *queue = (msg_type == MSG_TYPE_WORKER_DOWN) ? &state->waiting_tourists_down : &state->waiting_tourists_up;

    MsgBuf msg;
    while (state->is_running && worker_is_running) {
       
        if (msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long), msg_type, 0) == -1) {
            if (!worker_is_running || errno == EIDRM || errno == EINVAL || errno == EINTR) {
                break;
            }
            perror("Worker msgrcv");
            continue;
        }

        if (!state->is_running) break;

        switch (msg.action) {
            case ACTION_TOURIST_READY: {
                sem_wait(sem_state_mutex_id, 0);

                if (queue->count >= MAX_Q_SIZE) {
                    log_msg("%s: BŁĄD! Kolejka pełna (%d/%d). Turysta PID:%d zostaje odrzucony.", 
                        log_prefix, queue->count, MAX_Q_SIZE, msg.pid);
                    sem_signal(sem_state_mutex_id, 0);
                    break;
                }
                
                QueuedTourist new_tourist = { 
                    .pid = msg.pid, 
                    .type = msg.tourist_type, 
                    .age = msg.age, 
                    .requires_guardian = msg.requires_guardian 
                };

                queue->items[queue->tail] = new_tourist;
                queue->tail = (queue->tail + 1) % MAX_Q_SIZE;
                queue->count++;
                
                log_msg("%s: %s %d zgłosił się. Kolejka: %d osób", log_prefix, 
                    (new_tourist.type == BIKER ? "Rowerzysta" : "Pieszy"), new_tourist.pid, queue->count);
                
                sem_signal(sem_state_mutex_id, 0);
                break;
            }
            case ACTION_CHAIR_FREE:
                log_msg("%s: Otrzymano sygnał zwolnienia miejsca, próba wysłania grup. Kolejka: %d", 
                    log_prefix, queue->count);
                break;
        }
        
        if (is_paused) {
            log_msg("%s: Kolej wstrzymana (SIGUSR1). Czekanie na wznowienie (SIGUSR2)...", log_prefix);
            sem_wait(sem_state_mutex_id, 0);
            while (is_paused && state->is_running && worker_is_running) {
                sem_signal(sem_state_mutex_id, 0);
                sleep(1); // Czekaj na sygnał wznowienia
                sem_wait(sem_state_mutex_id, 0);
            }
            log_msg("%s: Wznowienie pracy (SIGUSR2).", log_prefix);
            sem_signal(sem_state_mutex_id, 0);
            continue;
        }
        
        if (msg_type == MSG_TYPE_WORKER_UP) {
            sem_wait(sem_state_mutex_id, 0);
            
            while (queue->count > 0) {
                QueuedTourist tourist = shared_q_pop(queue);
                
                int route = rand() % 3;  // 0=T1, 1=T2, 2=T3
                const char* route_name = (route == 0) ? "T1 (łatwa)" : (route == 1) ? "T2 (średnia)" : "T3 (trudna)";
                
                if (route == 0) stats->route_t1++;
                else if (route == 1) stats->route_t2++;
                else stats->route_t3++;
                
                log_msg("%s: Turystę %d wysyłam na trasę %s", log_prefix, tourist.pid, route_name);
                
                MsgBuf go_msg;
                go_msg.mtype = tourist.pid;
                go_msg.action = ACTION_TOURIST_READY;
                go_msg.tourist_type = tourist.type;
                msgsnd(msg_queue_id, &go_msg, sizeof(go_msg) - sizeof(long), 0);
            }
            
            sem_signal(sem_state_mutex_id, 0);
        } else {
            try_to_dispatch_groups(queue, log_prefix);
        }
    }

    log_msg("%s: Koniec pętli, uruchamiam finalne wysyłanie.", log_prefix);
    
    if (msg_type == MSG_TYPE_WORKER_DOWN) {
        try_to_dispatch_groups(queue, log_prefix);
    }
    
    log_msg("%s: Proces pracownika zakończony.", log_prefix);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Worker: Missing argument (up/down)\n");
        return 1;
    }

    signal(SIGTERM, worker_sigterm_handler);
    signal(SIGUSR1, worker_sigusr1_handler);
    signal(SIGUSR2, worker_sigusr2_handler);
    srand(time(NULL) ^ getpid());
    attach_ipc_resources();

    if (strcmp(argv[1], "down") == 0) {
        run_worker(MSG_TYPE_WORKER_DOWN, "PRACOWNIK (DÓŁ)");
    } else {
        run_worker(MSG_TYPE_WORKER_UP, "PRACOWNIK (GÓRA)");
    }

    return 0;
}
