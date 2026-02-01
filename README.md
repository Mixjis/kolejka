
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
| operacje.h   | Deklaracje funkcji                         |
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
[21:29:26.678] KRZESEŁKO(64555): Krzesełko #395 odjeżdża z pasażerami: [#657(R), #848(P), #842(P)] (R:1, P:2)
[21:29:26.678] KRZESEŁKO(64555): Krzesełko #396 odjeżdża z pasażerami: [#841(P)+2dz, #845(P)] (R:0, P:4)
[21:29:26.679] KRZESEŁKO(64555): Krzesełko #397 odjeżdża z pasażerami: [#849(R), #847(R)] (R:2, P:0)
...
[21:29:30.321] KRZESEŁKO(64556): Krzesełko #395 dotarło na górną stację z 3 pasażerami
[21:29:30.325] KRZESEŁKO(64556): Krzesełko #396 dotarło na górną stację z 2 pasażerami
[21:29:30.673] KRZESEŁKO(64556): Krzesełko #397 dotarło na górną stację z 2 pasażerami
...
```

**Oczekiwany rezultat:** Żadne krzesełko nie przekracza dozwolonych kombinacji.  
**Wynik:** logi pokazują poprawne kombinacje np. "[#123(R), #124(P)+1dz]" i symulacja przebiega dalej bez zacięcia.

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
**Obserwacje:**
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
```log
...
[21:28:07.766] TURYSTA(64766): Turysta #190 zakończył trasę zjazdową i dotarł na stację dolną (bilet #154)
[21:28:07.766] TURYSTA(64766): Turysta #190 - bilet czasowy wygasł!
[21:28:07.778] TURYSTA(64766): Turysta #190 kończy wizytę (przejazdy: 1)
...
[21:29:41.318] RAPORT: 3. KATEGORIE SPECJALNE:
[21:29:41.319] RAPORT:    VIP obsluzeni:            9
[21:29:41.319] RAPORT:    Dzieci z opiekunem:       64
[21:29:41.320] RAPORT:    Odrzuceni (wygasly):      146
...
```
**Oczekiwany rezultat:** Turyści z wygasłymi biletami są odrzucani, licznik rejected_expired rośnie.  
**Wynik:** logi pokazują "bilet czasowy wygasł!" i turysta opuszcza system, osoby ktorym wygasl bilet podczas pobytu widać w statystykach na końcu logów.

### Test 5: Limit osób na stacji (N=50)
**Cel:** Sprawdzenie czy limit STATION_CAPACITY jest przestrzegany.

**Procedura:** Przy ustawieniu długiego czasu awarii, turyści mogą się gromadzić na stacji dolnej bo nie są wpuszczani regularnie na peron. można wtedy zobaczyć stan pojemności.
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
----------
## 7. Linki do istotnych fragmentów kodu
