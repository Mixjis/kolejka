// main.c
#include <stdio.h>
#include <unistd.h>

#define TOTAL_CHAIRS 72
#define NUM_TOURISTS 30
#define SIM_DURATION 5

int main(void) {
    printf("SYMULACJA KOLEI LINOWEJ\n");
    printf("Krzesełka: %d, turyści: %d\n", TOTAL_CHAIRS, NUM_TOURISTS);
    printf("czas trwania: %d sekund\n", SIM_DURATION);

    sleep(SIM_DURATION);

    printf("Koniec.\n");


    return 0;
}
