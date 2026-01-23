#include "common.h"

volatile sig_atomic_t worker2_is_running = 1;
volatile sig_atomic_t emergency_stop_2 = 0;
int fifo_fd_2 = -1;

void worker2_sigterm_handler(int sig) {
    (void)sig;
    worker2_is_running = 0;
}

// SIGUSR1 - pracownik2 takze moze zatrzymac kolej
void worker2_sigusr1_handler(int sig) {
    (void)sig;
    emergency_stop_2 = 1;
}

void worker2_sigusr2_handler(int sig) {
    (void)sig;
    emergency_stop_2 = 0;
}

// Obsluga zatrzymania kolei inicjowanego przez pracownika2
void handle_emergency_stop_worker2() {
    log_msg("PRACOWNIK2: ZAGROZENIE! Zatrzymuje kolej (SIGUSR1).");

    sem_wait_op(sem_state_mutex_id, 0);
    state->is_paused = 1;
    sem_signal_op(sem_state_mutex_id, 0);

    // Powiadom pracownika1 przez FIFO
    if (fifo_fd_2 >= 0) {
        FifoMsg fmsg;
        fmsg.type = FIFO_PAUSE_REQ;
        fmsg.sender_pid = getpid();
        if (write(fifo_fd_2, &fmsg, sizeof(fmsg)) == -1) {
            perror("PRACOWNIK2: write FIFO_PAUSE_REQ");
        }
        log_msg("PRACOWNIK2: Wyslano PAUSE_REQ do pracownika1 przez FIFO.");
    }

    // Wyslij sygnal SIGUSR1 do pracownika1
    sem_wait_op(sem_state_mutex_id, 0);
    pid_t worker1_pid = state->worker_down_pid;
    sem_signal_op(sem_state_mutex_id, 0);

    if (worker1_pid > 0) {
        kill(worker1_pid, SIGUSR1);
        log_msg("PRACOWNIK2: Wyslano SIGUSR1 do pracownika1 (PID:%d).", worker1_pid);
    }

    log_msg("PRACOWNIK2: Kolej ZATRZYMANA. Czekam na SIGUSR2...");

    while (emergency_stop_2 && worker2_is_running) {
        usleep(100000);
    }

    if (worker2_is_running) {
        log_msg("PRACOWNIK2: Otrzymano SIGUSR2 - wznawiam.");

        // Wyslij potwierdzenie wznowienia
        if (fifo_fd_2 >= 0) {
            FifoMsg fmsg;
            fmsg.type = FIFO_RESUME_ACK;
            fmsg.sender_pid = getpid();
            if (write(fifo_fd_2, &fmsg, sizeof(fmsg)) == -1) {
                perror("PRACOWNIK2: write FIFO_RESUME_ACK");
            }
        }

        // Wyslij SIGUSR2 do pracownika1
        if (worker1_pid > 0) {
            kill(worker1_pid, SIGUSR2);
        }

        sem_wait_op(sem_state_mutex_id, 0);
        state->is_paused = 0;
        sem_signal_op(sem_state_mutex_id, 0);

        log_msg("PRACOWNIK2: Kolej WZNOWIONA.");
    }
}

// Obsluga FIFO - odpowiedz na zadania od pracownika1
void check_fifo_messages() {
    if (fifo_fd_2 < 0) return;

    FifoMsg fmsg;
    ssize_t r = read(fifo_fd_2, &fmsg, sizeof(fmsg));
    if (r <= 0) return;

    switch (fmsg.type) {
        case FIFO_PAUSE_REQ:
            log_msg("PRACOWNIK2: Otrzymano PAUSE_REQ od pracownika1. Potwierdzam.");
            {
                FifoMsg ack;
                ack.type = FIFO_PAUSE_ACK;
                ack.sender_pid = getpid();
                if (write(fifo_fd_2, &ack, sizeof(ack)) == -1) {
                    perror("PRACOWNIK2: write FIFO_PAUSE_ACK");
                }
            }
            break;
        case FIFO_RESUME_REQ:
            log_msg("PRACOWNIK2: Otrzymano RESUME_REQ od pracownika1. Potwierdzam.");
            {
                FifoMsg ack;
                ack.type = FIFO_RESUME_ACK;
                ack.sender_pid = getpid();
                if (write(fifo_fd_2, &ack, sizeof(ack)) == -1) {
                    perror("PRACOWNIK2: write FIFO_RESUME_ACK");
                }
            }
            break;
        default:
            break;
    }
}

// Glowna petla pracownika2 (stacja gorna)
void run_worker_up() {
    log_msg("PRACOWNIK2: Proces pracownika stacji gornej uruchomiony (PID:%d).", getpid());

    sem_wait_op(sem_state_mutex_id, 0);
    state->worker_up_pid = getpid();
    sem_signal_op(sem_state_mutex_id, 0);

    // Otworz FIFO
    fifo_fd_2 = open(FIFO_WORKER_PATH, O_RDWR | O_NONBLOCK);
    if (fifo_fd_2 == -1) {
        perror("PRACOWNIK2: open FIFO");
    }

    MsgBuf msg;

    while (state->is_running && worker2_is_running) {
        // Sprawdz awaryjne zatrzymanie
        if (emergency_stop_2) {
            handle_emergency_stop_worker2();
            if (!worker2_is_running) break;
        }

        // Sprawdz wiadomosci FIFO od pracownika1
        check_fifo_messages();

        // Odbierz wiadomosci od turystow docierajacych na gore
        ssize_t result = msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long),
                                MSG_TYPE_WORKER_UP, IPC_NOWAIT);

        if (result == -1) {
            if (errno == ENOMSG) {
                usleep(100000);
                continue;
            }
            if (!worker2_is_running || errno == EIDRM || errno == EINVAL) break;
            continue;
        }

        if (msg.action == ACTION_ARRIVED_TOP) {
            sem_wait_op(sem_state_mutex_id, 0);
            state->people_on_top++;
            int pop = state->people_on_top;
            sem_signal_op(sem_state_mutex_id, 0);

            log_msg("PRACOWNIK2: Turysta %d dotarl na gore. Osoby na gorze: %d",
                    msg.tourist_id, pop);

            // Przydziel droge wyjscia (jedna z 2 drog)
            int exit_route = msg.tourist_id % EXIT_ROUTES;

            // Otworz bramke wyjsciowa
            sem_wait_op(sem_exit_id, exit_route);
            log_msg("PRACOWNIK2: Turysta %d wychodzi droga %d.", msg.tourist_id, exit_route + 1);
            usleep(200000); // czas przejscia
            sem_signal_op(sem_exit_id, exit_route);

            sem_wait_op(sem_state_mutex_id, 0);
            state->people_on_top--;
            sem_signal_op(sem_state_mutex_id, 0);

            // Wyslij potwierdzenie do turysty
            MsgBuf ack;
            memset(&ack, 0, sizeof(ack));
            ack.mtype = msg.pid;
            ack.action = ACTION_TOURIST_READY;
            if (msgsnd(msg_queue_id, &ack, sizeof(ack) - sizeof(long), 0) == -1) {
                perror("PRACOWNIK2: msgsnd ack to tourist");
            }
        }
    }

    if (fifo_fd_2 >= 0) close(fifo_fd_2);
    log_msg("PRACOWNIK2: Proces pracownika zakonczony.");
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    struct sigaction sa_term, sa_usr1, sa_usr2;

    sa_term.sa_handler = worker2_sigterm_handler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, NULL);

    sa_usr1.sa_handler = worker2_sigusr1_handler;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = 0;
    sigaction(SIGUSR1, &sa_usr1, NULL);

    sa_usr2.sa_handler = worker2_sigusr2_handler;
    sigemptyset(&sa_usr2.sa_mask);
    sa_usr2.sa_flags = 0;
    sigaction(SIGUSR2, &sa_usr2, NULL);

    signal(SIGALRM, SIG_IGN);
    srand(time(NULL) ^ getpid());
    attach_ipc_resources();

    run_worker_up();

    return 0;
}
