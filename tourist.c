#include "common.h"

int tourist_id;
int age;
TouristType type;
int is_vip;
int has_guardian_req;

void tourist_sigterm_handler(int sig) {
    exit(0);
}

void take_ride(long worker_msg_type, int direction) {
    //PODEJŚCIE DO PERONU
    int platform_gate = rand() % PLATFORM_GATES;
    log_msg("TURYSTA %d: Czekam na peron (bramka %d).\n", tourist_id, platform_gate);
    sem_wait(sem_platform_id, platform_gate);
    log_msg("TURYSTA %d: Podszedłem do peronu.\n", tourist_id);

    // Zgłoszenie gotowości pracownikowi
    MsgBuf ready_msg;
    ready_msg.mtype = worker_msg_type;
    ready_msg.action = ACTION_TOURIST_READY;
    ready_msg.pid = getpid();
    ready_msg.tourist_type = type;
    ready_msg.age = age;
    ready_msg.requires_guardian = has_guardian_req;
    msgsnd(msg_queue_id, &ready_msg, sizeof(ready_msg) - sizeof(long), 0);

    sem_signal(sem_platform_id, platform_gate);
    log_msg("TURYSTA %d: Zwolniłem bramkę, czekam na zgłoszenie pracownika.\n", tourist_id);

    //CZEKANIE NA WEJŚCIE DO KRZESEŁKA
    log_msg("TURYSTA %d: Czekam na sygnał do wejścia na krzesełko.\n", tourist_id);
    MsgBuf go_msg;
    if (msgrcv(msg_queue_id, &go_msg, sizeof(go_msg) - sizeof(long), getpid(), 0) == -1) {
        exit(0);
    }
    
    log_msg("TURYSTA %d: Dostałem zgodę, wsiadam na przejazd %s!\n", tourist_id, direction == 1 ? "W GÓRĘ" : "W DÓŁ");

    //PRZEJAZD
    //sleep(RIDE_DURATION);
}


int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Tourist: Missing ID argument\n");
        return 1;
    }
    tourist_id = atoi(argv[1]);
    
    signal(SIGTERM, tourist_sigterm_handler);
    srand(time(NULL) ^ getpid());

    attach_ipc_resources();

    // Inicjalizacja atrybutów turysty
    age = (rand() % (AGE_MAX - AGE_MIN + 1)) + AGE_MIN;
    
    // Walidacja danych
    if (age < AGE_MIN || age > AGE_MAX) {
        fprintf(stderr, "TURYSTA %d: Błąd - wiek poza zakresem (%d)\n", tourist_id, age);
        exit(1);
    }
    
    type = (rand() % 2 == 0) ? WALKER : BIKER;
    is_vip = (rand() % 100) < VIP_PROBABILITY;
    has_guardian_req = (age < CHILD_GUARDIAN_REQ_AGE);

    log_msg("TURYSTA %d: Start (wiek: %d, typ: %s, vip: %d)\n", tourist_id, age, type == BIKER ? "Rowerzysta" : "Pieszy", is_vip);

    //KUPNO BILETU
    MsgBuf ticket_req;
    ticket_req.mtype = MSG_TYPE_CASHIER_REQ;
    ticket_req.tourist_id = tourist_id;
    ticket_req.pid = getpid();
    ticket_req.age = age;
    msgsnd(msg_queue_id, &ticket_req, sizeof(ticket_req) - sizeof(long), 0);
    
    log_msg("TURYSTA %d: Czekam na bilet...\n", tourist_id);
    sem_wait(sem_ticket_bought_id, tourist_id - 1);
    log_msg("TURYSTA %d: Bilet kupiony.\n", tourist_id);

    if (has_guardian_req) {
        log_msg("TURYSTA %d: Jestem dzieckiem, potrzebuję opiekuna.\n", tourist_id);
        sem_wait(sem_state_mutex_id, 0);
        stats->with_guardian++;
        sem_signal(sem_state_mutex_id, 0);
    }

    //WEJŚCIE NA STACJĘ
    int gate = -1; // -1 VIP
    if (!is_vip) {
        gate = rand() % ENTRY_GATES;
        log_msg("TURYSTA %d: Czekam na wejście (bramka %d).", tourist_id, gate);
        sem_wait(sem_entry_id, gate);
    } else {
        log_msg("TURYSTA %d: Wejście VIP (omijam kolejkę do bramek).", tourist_id);
        sem_wait(sem_state_mutex_id, 0);
        stats->vip_served++;
        sem_signal(sem_state_mutex_id, 0);
        gate = -1;
    }

    //WAŻNOŚć BILETU
    sem_wait(sem_state_mutex_id, 0);
    Ticket my_ticket = tickets[tourist_id];
    sem_signal(sem_state_mutex_id, 0);

    if (my_ticket.valid_until != -1 && time(NULL) > my_ticket.valid_until) {
        log_msg("TURYSTA %d: Bilet stracił ważność. Opuszczam kolejkę.", tourist_id);
        if (gate != -1) {
            sem_signal(sem_entry_id, gate);
        }
        exit(0);
    }

    if (!is_vip) {
        log_msg("TURYSTA %d: [PRZEJŚCIE] Przeszedłem bramkę wejściową %d.", tourist_id, gate);
        sem_signal(sem_entry_id, gate);
    } else {
        log_msg("TURYSTA %d: [PRZEJŚCIE] Wszedłem jako VIP.", tourist_id);
    }
    
    log_msg("TURYSTA %d: Czekam na miejsce na stacji.", tourist_id);
    sem_wait(sem_station_id, 0);


    sem_wait(sem_state_mutex_id, 0);
    state->station_population++;
    log_msg("TURYSTA %d: [PRZEJŚCIE] Wszedłem na teren dolnej stacji. Populacja: %d/%d", tourist_id, state->station_population, MAX_STATION_CAPACITY);
    sem_signal(sem_state_mutex_id, 0);

    //PODRÓŻ W GÓRĘ
    log_msg("TURYSTA %d: Rozpoczynam podróż W GÓRĘ.\n", tourist_id);
    take_ride(MSG_TYPE_WORKER_DOWN, 1);  // Czeka na pracownika DÓŁ

    //WYJŚCIE NA GÓRZE
    log_msg("TURYSTA %d: Zakończyłem przejazd na GÓRĘ, wysiadam.\n", tourist_id);
    
    //Na górze zwolnienie krzesełka
    sem_wait(sem_state_mutex_id, 0);
    if (state->busy_chairs > 0) {
        state->busy_chairs--;
    } else {
        log_msg("TURYSTA %d: BŁĄD! busy_chairs już 0 lub ujemne!", tourist_id);
    }
    sem_signal(sem_state_mutex_id, 0);
    
    // Powiadomienie pracownika NA GÓRZE
    MsgBuf free_msg_up;
    free_msg_up.mtype = MSG_TYPE_WORKER_UP;
    free_msg_up.action = ACTION_TOURIST_READY;
    free_msg_up.pid = getpid();
    free_msg_up.tourist_type = type;
    msgsnd(msg_queue_id, &free_msg_up, sizeof(free_msg_up) - sizeof(long), 0);

    //POBYT NA GÓRZE
    int time_on_top = (rand() % 9) + 2;
    log_msg("TURYSTA %d: Jestem na szczycie. Spędzę tu %d sekund.\n", tourist_id, time_on_top);
    //sleep(time_on_top);

    // Czekanie na sygnał od pracownika GÓRĘ
    log_msg("TURYSTA %d: Czekam na pracownika GÓRĘ, aby przebiec trasę i wrócić.\n", tourist_id);
    MsgBuf route_msg;
    if (msgrcv(msg_queue_id, &route_msg, sizeof(route_msg) - sizeof(long), getpid(), 0) == -1) {
        exit(0);
    }
    
    log_msg("TURYSTA %d: Przebiegłem trasę zjazdową, teraz jadę W DÓŁ.\n", tourist_id);

    //PODRÓŻ W DÓŁ
    log_msg("TURYSTA %d: Rozpoczynam podróż W DÓŁ.\n", tourist_id);
    take_ride(MSG_TYPE_WORKER_UP, 0);
    
    //WYJŚCIE NA DOLE
    log_msg("TURYSTA %d: Zakończyłem przejazd na DÓŁ, wysiadam.\n", tourist_id);
    
    sem_wait(sem_state_mutex_id, 0);
    if (state->busy_chairs > 0) {
        state->busy_chairs--;
    } else {
        log_msg("TURYSTA %d: BŁĄD! busy_chairs już 0 lub ujemne!", tourist_id);
    }
    state->station_population--; 
    sem_signal(sem_state_mutex_id, 0);
    sem_signal(sem_station_id, 0);

    // Powiadom pracownika NA DOLE
    MsgBuf free_msg_down;
    free_msg_down.mtype = MSG_TYPE_WORKER_DOWN;
    free_msg_down.action = ACTION_CHAIR_FREE;
    msgsnd(msg_queue_id, &free_msg_down, sizeof(free_msg_down) - sizeof(long), 0);

    log_msg("TURYSTA %d: Opuszczam system.\n", tourist_id);

    return 0;
}