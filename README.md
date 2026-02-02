
# Raport z Projektu: Symulacja Kolei Linowej

## Temat 4 – Kolej linowa

**Autor:** Michał Janus  
**Numer albumu:** 156716  
**Repozytorium GitHub:** https://github.com/Mixjis/kolejka

----------

## 1. Założenia projektowe

### 1.1. Opis projektu


> W pewnej górskiej miejscowości znajduje się krzesełkowa kolej linowa czynna w okresie letnim, obsługująca m. in. rowerowe trasy zjazdowe. Z kolei krzesełkowej korzystają piesi oraz rowerzyści. Kolej składa się z 4‑osobowych krzesełek o łącznej liczbie 72 sztuk. Jednocześnie może być zajętych 36 krzesełek. Na krzesełku może maksymalnie siedzieć dwóch rowerzystów lub jeden rowerzysta i dwóch pieszych, albo czterech pieszych.

> Rowerzyści/turyści przychodzą na teren stacji w losowych momentach czasu (nie wszyscy z nich muszą korzystać z kolei linowej). Wejście na teren kolejki linowej odbywa się po opłaceniu biletu/karnetu w kasie. Karnety są jednorazowe, czasowe (Tk1, Tk2, Tk3) lub dzienne. Dzieci poniżej 10. roku życia oraz seniorzy powyżej 65. roku życia mają 25% zniżki. Dzieci poniżej 8. roku życia znajdują się pod stałą opieką osoby dorosłej. Aby dostać się na peron i usiąść na krzesełku należy przejść dwa rodzaje bramek:

- wejście na teren dolnej stacji odbywa się czterema bramkami jednocześnie – na tych bramkach sprawdzany jest bilet/karnet,
- wejście na peron odbywa się trzema bramkami – na tych bramkach następuje kontrola osób, które mają usiąść na jednym krzesełku; bramki te otwiera pracownik1.

> Na terenie dolnej stacji (po przejściu pierwszych bramek, a przed wyjściem drugimi bramkami) może przebywać maksymalnie N osób. 
> Wyjazd/wyjście z peronu stacji górnej odbywa się dwoma drogami jednocześnie (ruch jednokierunkowy). 
> Stacja dolna jest obsługiwana przez pracownika1, stacja górna jest obsługiwana przez pracownika2. 
> W przypadku zagrożenia pracownik1 lub pracownik2 zatrzymują kolej linową (sygnał1). Aby wznowić działanie, pracownik który zatrzymał kolej komunikuje się z drugim pracownikiem – po otrzymaniu komunikatu zwrotnego o gotowości kolej jest uruchamiana ponownie (sygnał2).

> Rowerzyści zjeżdżają trzema trasami o różnym stopniu trudności – średni czas przejazdu dla poszczególnych tras jest różny i wynosi odpowiednio T1, T2 i T3 (T1 < T2 < T3).

### 1.2. Zasady działania systemu

**Zasady zajmowania krzesełek:**

-   Max 2 rowerzystów na krzesełku
-   Max 1 rowerzysta + 2 pieszych
-   Max 4 pieszych

**Typy biletów:**

-   Jednorazowy (SINGLE)
-   Czasowy TK1 (1h symulacyjna = 15s)
-   Czasowy TK2 (2h symulacyjna = 30s)
-   Czasowy TK3 (3h symulacyjna = 45s)
-   Dzienny (DAILY)

**Zniżki:** 25% dla dzieci poniżej 10 lat i seniorów powyżej 65 lat  
**VIP:** ok. 1% turystów posiada status VIP i wchodzi bez kolejki (VIP_PERCENT)
**Czas działania:** WORK_START_TIME &rarr; WORK_END_TIME  
**Zagrożenie:** Komunikacja pracowników za pomocą SIGUSR1/SIGUSR2

----------



### 1.3. Istotne parametry konfiguracyjne (struktury.h)

| Parametr            | Wartość | Opis                          |
|---------------------|---------|-------------------------------|
| TOTAL_TOURISTS      | 10000   | Całkowita liczba turystów     |
| MAX_CHAIRS          | 72      | Liczba krzesełek              |
| MAX_ACTIVE_CHAIRS   | 36      | Max krzesełek w ruchu         |
| STATION_CAPACITY    | 50      | Max osób na stacji dolnej     |
| WORK_START_TIME     | 5       | Czas otwarcia kasy (Tp)       |
| WORK_END_TIME       | 120     | Czas zamknięcia bramek (Tk)   |
| CHAIR_TRAVEL_TIME   | 4s      | Czas przejazdu krzesełka      |

----------

## 2. Realizacja
### 2.1. Pliki projektu


| Plik         | Opis                                       | 
|--------------|--------------------------------------------|
| main.c       | Proces główny – koordynacja symulacji      |
| cashier.c    | Proces kasjera – sprzedaż biletów          |
| worker.c     | Pracownik stacji dolnej                    |
| worker2.c    | Pracownik stacji górnej                    |
| tourist.c    | Proces turysty                             |
| utils.c      | Funkcje pomocnicze IPC                     |
| logger.c     | System logowania                           |
| struktury.h  | Definicje struktur i stałych               |
| utils.h   | Deklaracje funkcji                         |
| logger.h     | Deklaracje logowania                       |
| Makefile     | Plik budowania                      	    |


### 2.2. Przepływ turysty
####  Przybicie turysty do systemu
- **Proces:** `fork()` + `execl()` w main.c tworzy osobny proces dla każdego turysty
- **Parametry:** ID, wiek, typ (pieszy/rowerzysta), status VIP, liczba dzieci
- **Wątki dzieci:** Dla turystów z dziećmi <8 lat tworzone są wątki dzieci

#### Kupno biletu
- **Lokalizacja:** Kasa (proces cashier.c)
- **Komunikacja:** Kolejka komunikatów (MSG_TOURIST_TO_CASHIER, typ=1)
- **Priorytet VIP:** VIP używa MSG_VIP_PRIORITY, obsługa bez kolejki
- **Typy biletów:** SINGLE, TK1 (15s), TK2 (30s), TK3 (45s), DAILY
- **Zniżki:** -25% dla dzieci <10 lat i seniorów >65 lat
- **Synchronizacja:** Semafor SEM_MAIN chroni pamięć dzieloną podczas aktualizacji statystyk
- **Wynik:** Turysta otrzymuje numer biletu/karnetu

#### Wejście na stację (4 bramki wejściowe)
- **Kontrola:** Semafor SEM_GATE_ENTRY (max 4 równocześnie)
- **Limit stacji:** Semafor SEM_STATION (max 50 osób na terenie stacji)
- **Weryfikacja biletu:** Sprawdzenie ważności (czy nie wygasł dla biletów czasowych)
- **Rejestracja:** Zapis ID karnetu i czasu przejścia w `shm->gate_registry`
- **Odmowa:** Jeśli bilet wygasł &rarr; licznik `rejected_expired++`, turysta opuszcza system

#### Przejście na peron (3 bramki peronowe)
- **Komunikacja:** MSG_TOURIST_TO_PLATFORM (typ=5) do worker.c
- **Zwolnienie:** `sem_podnies(SEM_STATION)` - Zwolnienie miejsca na stacji dolnej
- **Worker1:** Proces pracownika stacji dolnej grupuje turystów w odpowiednie zestawy
- **Zasady grupowania:**
  - Max 2 rowerzystów (R+R)
  - Max 1 rowerzysta + 2 pieszych (R+P+P)
  - Max 4 pieszych (P+P+P+P)
  - Dzieci <8 lat wymagają opiekuna dorosłego (max 2 dzieci na dorosłego)
- **Synchronizacja:** 
  - Semafor SEM_GATE_PLATFORM (3 bramki)
  - `pthread_mutex` dla listy oczekujących
- **Odpowiedź:** Worker1 wysyła MSG_PLATFORM_ACK z numerem krzesełka

#### Wsiadanie na krzesełko
- **Dostępność:** Semafor SEM_CHAIRS (max 36 aktywnych krzesełek)
- **Dane krzesełka:** Zapisywane w `shm->chairs[chair_id]`: lista pasażerów, typy, czas wyjazdu
- **Wątek krzesełka:** Worker1 tworzy detached thread (`pthread_create` + `PTHREAD_CREATE_DETACHED`)

#### Przejazd na górę
- **Czas:** CHAIR_TRAVEL_TIME
- **Awaria:** Wątek krzesełka monitoruje `shm->emergency_stop`
  - Przy SIGUSR1 → zatrzymanie, aktywne czekanie na `shm->emergency_stop = false`
  - Przy SIGUSR2 → wznowienie jazdy
- **Komunikat przybycia:** Po dotarciu wysyłane MSG_CHAIR_ARRIVAL (typ=10) do worker2.c
- **Zwolnienie:** `sem_podnies(SEM_CHAIRS)` po zakończeniu przejazdu

#### Wyjście na stacji górnej (2 wyjścia)
- **Worker2:** Proces pracownika stacji górnej odbiera MSG_CHAIR_ARRIVAL
- **Synchronizacja:** Semafor SEM_GATE_EXIT (2 wyjścia)
- **Detached thread:** Worker2 tworzy wątek dla każdego wyjścia pasażerów
- **Komunikacja:** MSG_TOURIST_EXIT (typ=11) dla każdego turysty

#### Dalszy przebieg

**A) Pieszy:**
- Opuszcza system
- Proces kończy się

**B) Rowerzysta:**
- **Wybór trasy:** Losowo T1/T2/T3 z różnymi czasami zjazdu
- **Symulacja zjazdu:** według wybranej trasy
- **Decyzja o ponownym przejeździe:**
  - 50% szans dla biletów TK1/TK2/TK3/DAILY (jeśli nie wygasły)
  - 0% dla biletu SINGLE
  - Sprawdzenie `shm->gates_closed` - jeśli bramki zamknięte → koniec
- **Powrót:** Jeśli decyzja TAK &rarr; przejście do (wejścia na stację)
- **Koniec:** NIE &rarr; proces kończy się

#### Obsługa błędów i zamknięcie
- **Aktywne czekanie:** Na każdym etapie sprawdzane `shutdown_flag` i `gates_closed`
- **Reaper thread:** Główny proces (main.c) zbiera zombie procesów turystów (`waitpid` w pętli)

### 2.3. Generowanie plików

System generuje podczas działania następujące pliki:
#### kolej_log.txt
- **Tworzenie:** Plik otwarty przy inicjalizacji systemu (`logger_init()`)
- **Synchronizacja:** Semafor SEM_LOG_FILE chroni dostęp do pliku
- **Zawartość:** 
  - Kolorowe logi zdarzeń w konsoli (ANSI)
  - Typy logów: LOG_CASHIER, LOG_GATE, LOG_CHAIR, LOG_WORKER, LOG_TOURIST, LOG_SYSTEM
  - Format: `[CZAS][TYP] wiadomość`

#### raport_karnetow.txt
- **Tworzenie:** Generowany przez funkcję `generuj_raport()` po zakończeniu symulacji
- **Synchronizacja:** Semafor SEM_REPORT chroni zapis raportu
- **Zawartość:**
  - raport o przejsciu przez bramki ( raport - bramka - czas )
  - Zbierane przez całą symulacje statystyki

----------

## 3. Mechanizmy IPC

### 3.1. Semafory (11 semaforów)

| Indeks | Nazwa | Wartość początkowa | Funkcja |
|--------|-------|-------------------|---------|
| 0 | SEM_MAIN | 1 | Mutex pamięci dzielonej |
| 1 | SEM_STATION | 50 | Limit osób na stacji |
| 2 | SEM_PLATFORM | 1 | Mutex peronu |
| 3 | SEM_CHAIRS | 36 | Dostępne krzesełka |
| 4 | SEM_GATE_ENTRY | 4 | Bramki wejściowe |
| 5 | SEM_GATE_PLATFORM | 3 | Bramki na peron |
| 6 | SEM_GATE_EXIT | 2 | Wyjścia górna stacja |
| 7 | SEM_EMERGENCY | 1 | Flaga awarii |
| 8 | SEM_WORKER_SYNC | 0 | Synchronizacja pracowników |
| 9 | SEM_LOG_FILE | 1 | Mutex pliku logów |
| 10 | SEM_REPORT | 1 | Mutex raportu |

### 3.2. Pamięć dzielona (SharedMemory)

Struktura `SharedMemory` zawiera:

-   Stan systemu (is_running, emergency_stop, gates_closed)
-   Statystyki sprzedaży biletów
-   Statystyki przejazdów
-   Liczniki turystów w różnych lokalizacjach
-   Dane krzesełek
-   Rejestr przejść przez bramki

### 3.3. Kolejki komunikatów

System wykorzystuje dwie kolejki komunikatów do komunikacji między procesami:

**Kolejka główna (MSG_KEY) - komunikacja turystów z systemem:**

| Typ | Nazwa | Kierunek | Opis |
|-----|-------|----------|------|
| 1 | MSG_TOURIST_TO_CASHIER | Turysta → Kasjer | Żądanie kupna biletu |
| 2 | MSG_CASHIER_TO_TOURIST | Kasjer → Turysta | Potwierdzenie sprzedaży biletu |
| 3 | MSG_TOURIST_TO_GATE | Turysta → Bramka | Żądanie wejścia przez bramkę |
| 4 | MSG_GATE_TO_TOURIST | Bramka → Turysta | Potwierdzenie przepuszczenia |
| 5 | MSG_TOURIST_TO_PLATFORM | Turysta → Worker1 | Żądanie wejścia na peron |
| 6 | MSG_PLATFORM_TO_TOURIST | Worker1 → Turysta | Przydzielenie krzesełka (MSG_PLATFORM_ACK) |
| 11 | MSG_TOURIST_EXIT | Worker2 → Turysta | Pozwolenie na wyjście z górnej stacji |
| 101 | MSG_VIP_PRIORITY | VIP → Kasjer | Priorytetowe żądanie biletu (1 + 100) |

**Kolejka pracowników (MSG_WORKER_KEY) - komunikacja między worker1 i worker2:**

| Typ | Nazwa | Kierunek | Opis |
|-----|-------|----------|------|
| 7 | MSG_WORKER_EMERGENCY | Worker1/2 | Sygnał awaryjnego zatrzymania kolei |
| 8 | MSG_WORKER_READY | Worker1/2 | Potwierdzenie gotowości do wznowienia |
| 9 | MSG_CHAIR_DEPARTURE | Worker1 | Odjazd krzesełka ze stacji dolnej |
| 10 | MSG_CHAIR_ARRIVAL | Worker1 → Worker2 | Przyjazd krzesełka na stację górną |

----------

## 4. Podsumowanie co udało się zrobić?

### 4.1. Elementy Główne

- **Sprzedaż biletów** - kasjer obsługuje różne typy biletów ze zniżkami  
- **System VIP** - priorytetowa kolejka dla VIP-ów  
- **Bramki wejściowe** - 4 bramki z kontrolą biletów  
- **Bramki na peron** - 3 bramki z kontrolą grup (worker1)  
- **Zasady krzesełek** - poprawna kombinacja rowerzystów/pieszych  
- **Trasy zjazdowe** - 3 trasy T1, T2, T3 z różnymi czasami
- **Ponowne przejazdy** - rowerzyści mogą wielokrotnie korzystać z biletów czasowych/dziennych 
- **Dzieci z opiekunem** - max 2 dzieci pod opieką dorosłego  
- **Zatrzymanie awaryjne** - sygnały SIGUSR1/SIGUSR2 między pracownikami  
- **Godziny pracy** - system Tp-Tk z zamykaniem bramek  
- **Raport końcowy** - generowanie podsumowania  
- **Rejestracja przejść** - zapis id karnetu i godziny  

### 4.2. Elementy dodatkowe

-   **Wątek sprzątający** - automatyczne zbieranie zakończonych procesów turystów
-   **Detached threads** - dla krzesełek i wyjść turystów
-   **Kolorowe logi** - ANSI colors w terminalu
----------

## 5. Problemy napotkane podczas realizacji

### 5.1. Synchronizacja podczas awarii
**Problem:** Podczas awaryjnego zatrzymania turyści próbowali wysyłać komunikaty, co powodowało zakleszczenie i nie zostawali nigdy wysłani.  
**Rozwiązanie:** Dodanie ciągłego odbierania komunikatów (`process_platform_messages()`) w worker1 aż do gotowości obu pracowników.

### 5.2. Blokady semaforów przy zamykaniu
**Problem:** Procesy turystów blokowały się na semaforach podczas zamykania systemu.  
**Rozwiązanie:** aktywne sprawdzanie flag `shutdown_flag` i `gates_closed`.

### 5.3. Race condition przy awariach
**Problem:** Worker1 i worker2 równocześnie modyfikowali emergency_stop, co powodowało problemy przy próbie wznowienia pracy kolei.  
**Rozwiązanie:** Synchronizacja awarii
- Inicjator zapisuje emergency_initiator w shm i wysyła SIGUSR1.
- Drugi worker ustawia worker?_ready=true i czeka.
- Inicjator po gotowości drugiego wysyła SIGUSR2 i zeruje awarię.

### 5.4. Zombie procesy turystów
**Problem:** Po zakończeniu procesów turystów pozostawały procesy zombie, które nie były zbierane przez proces główny.  
**Rozwiązanie:** Utworzenie dedykowanego wątku reaper_thread w main.c, który w pętli wywołuje waitpid(-1, &status, WNOHANG) i zbiera zakończone procesy potomne.

----------

## 6. Opis testów

### Test 1: Poprawność kombinacji na krzesełkach
**Cel:** Sprawdzenie, czy pracownik poprawnie grupuje osoby wchodzące na peron.  
**Obserwacje w logach:**
```log
...
[09:49:10.503] KRZESEŁKO(1175): Krzesełko #44 odjeżdża z pasażerami: [#25(P)+2dz, #61(P)] (R:0, P:4)
[09:49:14.263] KRZESEŁKO(1176): Krzesełko #44 dotarło na górną stację z 2 pasażerami
...
[09:49:47.907] KRZESEŁKO(1175): Krzesełko #184 odjeżdża z pasażerami: [#387(P), #388(P), #397(P), #400(P)] (R:0, P:4)
[09:49:53.343] KRZESEŁKO(1176): Krzesełko #184 dotarło na górną stację z 4 pasażerami

[09:49:56.155] KRZESEŁKO(1175): Krzesełko #220 odjeżdża z pasażerami: [#468(R), #470(P), #471(P)] (R:1, P:2)
[09:49:56.410] KRZESEŁKO(1175): Krzesełko #221 odjeżdża z pasażerami: [#472(R), #473(R)] (R:2, P:0)
...
[09:50:00.282] KRZESEŁKO(1176): Krzesełko #220 dotarło na górną stację z 3 pasażerami
[09:50:00.411] KRZESEŁKO(1176): Krzesełko #221 dotarło na górną stację z 2 pasażerami

...
```
**Oczekiwany rezultat:** Żadne krzesełko nie przekracza dozwolonych kombinacji.  
Możliwe maksymalne kombinacje:    
- 2 ROWERZYSTÓW  
- 4 PIESZYCH   
- 1 ROWERZYSTA + 2 PIESZYCH  
**Wynik:** logi pokazują poprawne kombinacje np. "[#472(R), #473(R)]" i symulacja przebiega dalej bez zacięcia.

### Test 2: Zatrzymanie awaryjne
**Cel:** Sprawdzenie mechanizmu awaryjnego zatrzymania i wznowienia.  
**Obserwacje w logach:**
```log
[21:27:54.090] AWARIA(64555): PRACOWNIK1: Inicjuję AWARYJNE ZATRZYMANIE kolei!
...
[21:27:54.357] AWARIA(64556): PRACOWNIK2: Odebrano sygnał AWARII od worker1!
...
[21:27:54.674] AWARIA(64556): PRACOWNIK2: Potwierdzam gotowość (awaria od worker1)
...
[21:27:59.255] AWARIA(64555): PRACOWNIK1: Worker2 gotowy - Zatrzymanie ruchu kolei...

(w tym miejscu pracownik1 nie wpuszcza nikogo na peron a kolej sie zatrzymuje na czas EMERGENCY_DURATION (5s))

[21:28:04.127] AWARIA(64555): PRACOWNIK1: Kolej WZNOWIONA - normalny ruch!
[21:28:04.127] KRZESEŁKO(64555): Krzesełko #90 WZNAWIA jazdę (pozostało: 1 s)
[21:28:04.127] KRZESEŁKO(64555): Krzesełko #107 WZNAWIA jazdę (pozostało: 3 s)
...
[21:28:04.181] AWARIA(64556): PRACOWNIK2: Otrzymano sygnał WZNOWIENIA od worker1!
```
**Oczekiwany rezultat:** Kolej zatrzymuje się, krzesełka w locie czekają, po wznowieniu kontynuują.  
**Wynik:** logi pokazują zatrzymanie i wznowienie krzesełek. symulacja przebiega dalej bez zacięcia.

### Test 3: Zamknięcie bramek (czas Tk)
**Cel:** Weryfikacja zachowania po osiągnięciu czasu Tk.  
**Obserwacje w logach:**
```log
[21:29:23.097] SYSTEM(64554): Osiągnięto czas Tk - zamykam bramki wejściowe
...
[21:29:23.181] TURYSTA(66643): Turysta #890 - bramki zamknięte, odchodzę
[21:29:23.179] TURYSTA(66635): Turysta #886 - bramki zamknięte, odchodzę
...
[21:29:23.746] KASJER(64557): Bramki zamknięte - odmowa dla turysty #896
...
[21:29:28.350] TURYSTA(66307): Turysta #763 - bilet czasowy wygasł!
[21:29:28.350] TURYSTA(66346): Turysta #780 zakończył trasę zjazdową i dotarł na stację dolną (bilet #737)
[21:29:28.350] PRACOWNIK1(64555): Wpuszczam turyste #389(R) na krzesełko
...
[21:29:38.005] PRACOWNIK2(64556): Turysta #878 zakończył zjazd trasą T3 (trudna) i zjeżdża na dół
[21:29:38.007] TURYSTA(66162): Turysta #703 kończy wizytę (przejazdy: 2)
[21:29:38.010] SYSTEM(64554): Brak aktywnych turystów
[21:29:38.010] SYSTEM(64554): Oczekiwanie 3 sekund przed wyłączeniem...
```

**Oczekiwany rezultat:** Bramki zamknięte, obsługa pozostałych turystów.  
**Wynik:** system kończy pracę dokańczając obsługę pozostałych turystów ktorzy zdazyli przejsc na peron. Zamykając bramki. A turyści którzy nie zdążyli wejść na peron rezygnują. 

### Test 4: Bilety czasowe - wygaśnięcie
**Cel:** Sprawdzenie odmowy wejścia z wygasłym biletem.  
**Obserwacje w logach:**
```log
...
[09:48:52.282] KASJER(1177): Sprzedano bilet #16 (CZASOWY TK1 (1h)) turyscie #35 (rowerzysta, 63 lat) - cena: 25
...
[09:49:09.754] TURYSTA(1213): Turysta #35 zakończył trasę zjazdową i dotarł na stację dolną (bilet #16)
[09:49:09.760] TURYSTA(1213): Turysta #35 - bilet czasowy wygasł!
[09:49:09.871] TURYSTA(1213): Turysta #35 kończy wizytę (przejazdy: 1)
...
[09:50:13.189] RAPORT: 3. KATEGORIE SPECJALNE:
[09:50:13.189] RAPORT:    VIP obsluzeni:            6
[09:50:13.189] RAPORT:    Dzieci z opiekunem:       41
[09:50:13.190] RAPORT:    Odrzuceni (wygasly):      93
...
```
**Oczekiwany rezultat:** Turyści z wygasłymi biletami są odrzucani, licznik rejected_expired rośnie.  
**Wynik:** logi pokazują "bilet czasowy wygasł!" i turysta opuszcza system, osoby ktorym wygasl bilet podczas pobytu widać w statystykach na końcu logów.

### Test 5: Limit osób na stacji (N=50)
**Cel:** Sprawdzenie czy limit STATION_CAPACITY jest przestrzegany.  
**Procedura:** Przy ustawieniu długiego czasu awarii, turyści mogą się gromadzić na stacji dolnej bo nie są wpuszczani na peron. można wtedy zobaczyć stan pojemności.  
**Obserwacje w logach:**  
```log
[22:19:48.290] SYSTEM(70335): 50/50 turystów na stacji dolnej
[22:19:48.362] TURYSTA(70335): Turysta #552 - pozostały czas biletu: 30 sekund
[22:19:48.366] TURYSTA(70335): Turysta #552 wpuszczony przez bramkę wejściową #1 (bilet #531)
[22:19:48.366] TURYSTA(70335): Turysta #552 wszedł na stację dolną (bilet #531, typ: pieszy)
[22:19:48.743] TURYSTA(70342): Turysta #558 przybywa (rowerzysta, 77 lat)
[22:19:48.821] KASJER(69142): Sprzedano bilet #533 (CZASOWY TK3 (3h)) turyscie #558 (rowerzysta, 77 lat) (ze zniżką 25%) - cena: 42
...
(tutaj już wiecej osób nie jest wpuszczanych aż do końca awarii na stacje dolną więc widać limit)
...
[22:20:05.433] AWARIA(69140): PRACOWNIK1: Kolej WZNOWIONA - normalny ruch!
...
[22:20:05.468] SYSTEM(70339): 43/50 turystów na stacji dolnej
```
**Oczekiwany rezultat:** Max 50 osób.  
**Wynik:** semafor SEM_STATION limituje dostęp i nie przekracza 50,  

### Test 6: Zniżki dla dzieci i seniorów
**Cel:** Weryfikacja poprawności naliczania opłat w zależności od atrybutu wieku turysty.  
25% dla dzieci poniżej 10 lat i seniorów powyżej 65 lat
Ceny aktualnie ustawione:
```C
#define PRICE_SINGLE         10
#define PRICE_TK1            25
#define PRICE_TK2            40
#define PRICE_TK3            55
#define PRICE_DAILY          80
#define DISCOUNT_PERCENT     25
```  
**Obserwacje w logach:**  
```log
[22:53:57.017] KASJER(70798): Sprzedano bilet #6 (CZASOWY TK2 (2h)) turyscie #181 (rowerzysta, 43 lat) - cena: 40
[22:53:57.090] KASJER(70798): Sprzedano bilet #8 (CZASOWY TK2 (2h)) turyscie #80 (rowerzysta, 71 lat) (ze zniżką 25%) - cena: 30
....
[22:53:57.477] KASJER(70798): Sprzedano bilet #34 (CZASOWY TK3 (3h)) turyscie #64 (pieszy, 36 lat) - cena: 55
[22:53:57.478] KASJER(70798):   -> Turysta #64 ma pod opieką 1 dzieci (bilety ze zniżką 25%, suma za dzieci: 42)
...
[22:54:03.362] KASJER(70798): Sprzedano bilet #170 (CZASOWY TK2 (2h)) turyscie #75 (pieszy, 9 lat) (ze zniżką 25%) - cena: 30
```
**Oczekiwany rezultat:**
- Seniorzy (>70 lat): Naliczenie zniżki 25% (np. dla Tk2 cena 30 zamiast 40).
- Dzieci (<10 lat): Naliczenie zniżki 25% (np. dla Tk2 cena 30 zamiast 40).
- Dorośli: Cena standardowa. (np. Dla Tk3 cena 55).  

**Wynik:** Logi potwierdzają przydzielanie zniżek dla odpowiednich osób.

### Test 7: Turyści niekorzystający z kolei
**Cel:** Sprawdzenie czy nie wszyscy turyści muszą korzystać z kolei.  
**Obserwacje w logach:**  
```log
[09:48:43.952] TURYSTA(1208): Turysta #30 przybywa (rowerzysta, 59 lat)
[09:48:43.954] TURYSTA(1208): Turysta #30 tylko ogląda i odchodzi
```
**Oczekiwany rezultat:** Część turystów rezygnuje bez blokowania systemu.  
**Wynik:** Turyści mogą odejść bez kupowania biletu.

### Test 8: Wielokrotne przejazdy z biletem czasowym/dziennym
**Cel:** Weryfikacja możliwości ponownego przejazdu.  
**Obserwacje w logach:**   
```log
[09:48:52.986] KASJER(1177): Sprzedano bilet #27 (CZASOWY TK2 (2h)) turyscie #132 (rowerzysta, 20 lat) - cena: 40
...
[09:49:08.099] PRACOWNIK2(1176): Turysta #132 zakończył zjazd trasą T1 (łatwa) i zjeżdża na dół
...
[09:49:41.176] PRACOWNIK2(1176): Turysta #132 zakończył zjazd trasą T3 (trudna) i zjeżdża na dół
...
[09:49:41.579] TURYSTA(1313): Turysta #132 zakończył trasę zjazdową i dotarł na stację dolną (bilet #27)
[09:49:41.594] TURYSTA(1313): Turysta #132 - bilet czasowy wygasł!
[09:49:41.658] TURYSTA(1313): Turysta #132 kończy wizytę (przejazdy: 2)
...
[09:50:05.043] TURYSTA(1365): Turysta #178 kończy wizytę (przejazdy: 3)
```
**Oczekiwany rezultat:** Rowerzyści z biletami TK/DAILY mogą wykonać >1 przejazd.  
**Wynik:** Logi pokazują turystów z liczbą przejazdów > 1.


----------
## 7. Linki do istotnych fragmentów kodu

### a) Tworzenie i obsługa plików (creat(), open(), close(), read(), write(), unlink())

| Funkcja | Plik | Opis | Link |
|---------|------|------|------|
| `open()` | logger.c | Otwarcie pliku logów | [logger.c#L82](https://github.com/Mixjis/kolejka/blob/main/src/logger.c#L82) |
| `write()` | logger.c | Zapis do pliku | [logger.c#L59](https://github.com/Mixjis/kolejka/blob/main/src/logger.c#L59) |
| `close()` | logger.c | Zamknięcie pliku logów | [logger.c#L129](https://github.com/Mixjis/kolejka/blob/main/src/logger.c#L129) |

---

### b) Tworzenie procesów (fork(), exec(), exit(), wait())

| Funkcja | Plik | Opis | Link |
|---------|------|------|------|
| `fork()` + `execl()` | main.c | Tworzenie procesów turystów | [main.c#L93-L116](https://github.com/Mixjis/kolejka/blob/main/src/main.c#L93-L116) |
| `fork()` + `execl()` | main.c | Uruchomienie procesu kasjera | [main.c#L209-L215](https://github.com/Mixjis/kolejka/blob/main/src/main.c#L209-L215) |
| `fork()` + `execl()` | main.c | Uruchomienie procesów pracowników (worker1, worker2) | [main.c#L192-L206](https://github.com/Mixjis/kolejka/blob/main/src/main.c#L192-L206) |
| `exit()` | utils.c | Zakończenie po błędzie  | [utils.c#L27](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L27) |
| `waitpid()` | main.c | Wątek reaper zbierający procesy zombie | [main.c#L53](https://github.com/Mixjis/kolejka/blob/main/src/main.c#L53) |

---

### c) Tworzenie i obsługa wątków

| Funkcja | Plik | Opis | Link |
|---------|------|------|------|
| `pthread_create()` | worker.c | Tworzenie wątków krzesełek | [worker.c#L463](https://github.com/Mixjis/kolejka/blob/main/src/worker.c#L463) |
| `pthread_mutex_lock/unlock()` | worker2.c | mutex dla bramek wyjściowych | [worker2.c#L97-L103](https://github.com/Mixjis/kolejka/blob/main/src/worker2.c#L97-L103) |
| `pthread_create()` | tourist.c | tworzenie wątku dziecka | [tourist.c#L85](https://github.com/Mixjis/kolejka/blob/main/src/tourist.c#L85) |
| `pthread_join()` | tourist.c | Oczekiwanie na zakończenie wątków dzieci | [tourist.c#L102](https://github.com/Mixjis/kolejka/blob/main/src/tourist.c#L102) |

---

### d) Obsługa sygnałów (kill(), signal(), sigaction())

| Funkcja | Plik | Opis | Link |
|---------|------|------|------|
| `sigaction()` | worker.c | Rejestracja handlera SIGUSR1/SIGUSR2 | [worker.c#L485-L487](https://github.com/Mixjis/kolejka/blob/main/src/worker.c#L485-L487) |
| `sigaction()` | worker2.c | Rejestracja handlera SIGUSR1/SIGUSR2 | [worker2.c#L213-L215](https://github.com/Mixjis/kolejka/blob/main/src/worker2.c#L213-L215) |
| `kill()` | worker.c | Wysyłanie sygnału awarii do worker2 | [worker.c#L304](https://github.com/Mixjis/kolejka/blob/main/src/worker.c#L304) |
| `kill()` | worker2.c | Wysyłanie sygnału awarii do worker1 | [worker2.c#L46](https://github.com/Mixjis/kolejka/blob/main/src/worker2.c#L46) |
|`signal()`| mainc.c | Ustawienie Ignorowania sygnału | [main.c#L159-L160](https://github.com/Mixjis/kolejka/blob/main/src/main.c#L159-L160) |
---

### e) Synchronizacja procesów - Semafory (ftok(), semget(), semctl(), semop())

| Funkcja | Plik | Opis | Link |
|---------|------|------|------|
| `ftok()` | utils.c | Generowanie klucza IPC | [utils.c#L24](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L24) |
| `semget()` | utils.c | Tworzenie semaforów | [utils.c#L35](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L35) |
| `semctl()` | utils.c | Inicjalizacja wartości semaforów | [utils.c#L42](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L42) |
| `semop()` | utils.c | Funkcje sem_opusc() i sem_podnies() | [utils.c#L156](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L156) |

---

### f) Łącza (mkfifo(), pipe(), dup(), dup2(), popen())

> Projekt nie wykorzystuje Łącz nazwanych ani nienazwanych

---

### g) Segmenty pamięci dzielonej (ftok(), shmget(), shmat(), shmdt(), shmctl())

| Funkcja | Plik | Opis | Link |
|---------|------|------|------|
| `ftok()` | utils.c | Generowanie klucza pamięci dzielonej | [utils.c#L469](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L469) |
| `shmget()` | utils.c | Tworzenie segmentu SharedMemory | [utils.c#L471](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L471) |
| `shmat()` | utils.c | Dołączanie pamięci dzielonej | [utils.c#L302](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L302) |
| `shmdt()` | main.c | Odłączanie pamięci przy zamykaniu | [utils.c#L311](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L311) |
| `shmctl()` | main.c | Usuwanie segmentu po zakończeniu | [utils.c#L472](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L472) |
| Struktura SharedMemory | struktury.h | Definicja struktury pamięci dzielonej | [struktury.h#L162-L236](https://github.com/Mixjis/kolejka/blob/main/src/struktury.h#L162-L236) |

---

### h) Kolejki komunikatów (ftok(), msgget(), msgsnd(), msgrcv(), msgctl())

| Funkcja | Plik | Opis | Link |
|---------|------|------|------|
| `ftok()` | utils.c | Generowanie kluczy kolejek | [utils.c#L483](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L483) |
| `msgget()` | utils.c | Tworzenie kolejki IPC_KEY_MSG_WORKER | [utils.c#L340](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L340) |
| `msgsnd()` | tourist.c | Wysyłanie komunikatu | [utils.c#L386](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L386) |
| `msgrcv()` | cashier.c | Odbiór komunikatu | [utils.c#L397](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L397) |
| `msgctl()` | main.c | Usuwanie kolejek | [utils.c#L486](https://github.com/Mixjis/kolejka/blob/main/src/utils.c#L486) |

---

### i) Gniazda (socket(), bind(), listen(), accept(), connect())

> Projekt nie wykorzystuje gniazd sieciowych 

---

## Pliki projektu
```
├──Makefile
└──src
    ├── main.c           # Główny proces
    ├── cashier.c        # Proces kasjera
    ├── worker.c         # Pracownik stacji dolnej
    ├── worker2.c        # Pracownik stacji górnej
    ├── tourist.c        # Proces turysty
    ├── utils.c          # Funkcje pomocnicze IPC
    ├── logger.c         # System logowania
    ├── struktury.h      # Definicje struktur
    ├── utils.h       # Deklaracje funkcji
    └── logger.h         # Deklaracje logowania
```