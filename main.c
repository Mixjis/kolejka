// main.c
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>


#define TOTAL_CHAIRS 72
#define NUM_TOURISTS 30
#define SIM_DURATION 5

typedef struct {
    int is_running;
    int busy_chairs;
    int people_in_station;
    int stop_selling;
    time_t start_time;
    time_t end_time;
    int emergency_stop;
    int total_passes;
} StationState;

typedef struct {
    int total_tourists;
    int total_rides;
    int vip_served;
    int children_served;
    int bikers;
    int walkers;
} DailyStats;

char log_filename[] = "kolej_log.txt";
FILE *log_file;

void log_event(const char *format, ...) {
    va_list args;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[10];
    strcpy(timestamp, ctime(&now)); 
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    
    FILE *f = fopen(log_filename, "a");
    if (f) {
        fprintf(f, "[%s] ", timestamp);
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }
}

int main(void) {

    srand(time(NULL));
    
    log_file = fopen(log_filename, "w");
    if (!log_file) { 
        perror("fopen log");
        return 1;
    }
    fclose(log_file);

    printf("SYMULACJA KOLEI LINOWEJ\n");
    printf("PLIK Z LOGAMI: %s\n\n", log_filename);

    StationState state = {0};
    DailyStats stats = {0};
    
    state.is_running = 1;
    state.start_time = time(NULL);
    state.end_time = state.start_time + SIM_DURATION;

    char time_str[9];
    strftime(time_str, sizeof(time_str), "%T", localtime(&state.start_time));

    log_event("------------------------------------------");
    log_event("Inicjalizacja");
    log_event("Czas rozpoczecia: %s", time_str);
    log_event("Liczba krzeslek: %d", TOTAL_CHAIRS);
    log_event("------------------------------------------");
    
    printf("Start time: %s\n", time_str);
    printf("czas trwania: %d sekund\n", SIM_DURATION);

    // printf("Krzesełka: %d, turyści: %d\n", TOTAL_CHAIRS, NUM_TOURISTS);
    // printf("czas trwania: %d sekund\n", SIM_DURATION);

    sleep(SIM_DURATION);

    printf("Koniec.\n");


    return 0;
}
