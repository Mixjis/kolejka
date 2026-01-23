#include "common.h"

FILE *log_file = NULL;

void logger_sigterm_handler(int sig) {
    if (log_file) {
        fprintf(log_file, "LOGGER: Otrzymano SIGTERM, zamykanie pliku logu.\n");
        fflush(log_file);
        fclose(log_file);
    }
    exit(0);
}

int main(void) {
    signal(SIGTERM, logger_sigterm_handler);

    attach_ipc_resources();

    log_file = fopen("kolej_log.txt", "a");
    if (!log_file) {
        perror("Logger fopen");
        exit(1);
    }
    setvbuf(log_file, NULL, _IOLBF, 0);

    fprintf(log_file, "LOGGER: Proces loggera uruchomiony.\n");
    fflush(log_file);

    MsgBuf msg;
    while (1) {
        if (msgrcv(msg_queue_id, &msg, sizeof(msg) - sizeof(long), MSG_TYPE_LOG, 0) == -1) {
            if (errno == EIDRM || errno == EINVAL) {
                break;
            }
            perror("Logger msgrcv");
            continue;
        }

        time_t now = time(NULL);
        char time_str[20];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

        fprintf(log_file, "[%s] [PID: %d] %s\n", time_str, msg.pid, msg.mtext);
        fflush(log_file);
    }
    
    fprintf(log_file, "LOGGER: Zakończono pętlę, zamykanie.\n");
    fflush(log_file);
    if (log_file) {
        fclose(log_file);
    }

    return 0;
}
