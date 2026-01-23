#include "common.h"

FILE *log_file = NULL;
volatile sig_atomic_t logger_running = 1;

void logger_sigterm_handler(int sig) {
    (void)sig;
    logger_running = 0;
}

int main(void) {
    struct sigaction sa;
    sa.sa_handler = logger_sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGINT, SIG_IGN);  // SIGINT obsluguje main, logger czeka na SIGTERM

    attach_ipc_resources();

    log_file = fopen("kolej_log.txt", "a");
    if (!log_file) {
        perror("Logger fopen");
        exit(1);
    }
    setvbuf(log_file, NULL, _IOLBF, 0);

    fprintf(log_file, "=== LOGGER URUCHOMIONY (PID:%d) ===\n", getpid());
    fflush(log_file);

    MsgBuf msg;
    while (logger_running) {
        ssize_t result = msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long),
                                MSG_TYPE_LOG, IPC_NOWAIT);

        if (result == -1) {
            if (errno == ENOMSG) {
                usleep(50000);
                continue;
            }
            if (errno == EIDRM || errno == EINVAL) {
                break;
            }
            if (errno != EINTR) {
                perror("Logger msgrcv");
            }
            continue;
        }

        time_t now = time(NULL);
        char time_str[20];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

        fprintf(log_file, "[%s] [PID:%d] %s\n", time_str, msg.pid, msg.mtext);
        fflush(log_file);
    }

    fprintf(log_file, "=== LOGGER ZAKONCZONY ===\n");
    fflush(log_file);
    if (log_file) {
        fclose(log_file);
    }

    return 0;
}
