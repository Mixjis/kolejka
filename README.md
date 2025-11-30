
# **Temat projektu:** 4. Kolej linowa
**Autor:** Michał Janus  
**Nr albumu:** 156716  
**Link do repozytorium:** https://github.com/Mixjis/kolejka  

## 1. Opis zadania

Temat 4 – Kolej linowa.  
W pewnej górskiej miejscowości znajduje się krzesełkowa kolej linowa czynna w okresie letnim, obsługująca m.in. rowerowe trasy zjazdowe. Z kolei krzesełkowej korzystają piesi oraz rowerzyści. Kolej składa się z 4‑osobowych krzesełek o łącznej liczbie 72 sztuk. Jednocześnie może być zajętych 36 krzesełek. Na krzesełku może maksymalnie siedzieć dwóch rowerzystów lub jeden rowerzysta i dwóch pieszych, albo czterech pieszych.

Rowerzyści/turyści przychodzą na teren stacji w losowych momentach czasu (nie wszyscy z nich muszą korzystać z kolei linowej). Wejście na teren kolejki linowej odbywa się po opłaceniu biletu/karnetu w kasie. Karnety są jednorazowe, czasowe (Tk1, Tk2, Tk3) lub dzienne. Dzieci poniżej 10. roku życia oraz seniorzy powyżej 65. roku życia mają 25% zniżki. Dzieci poniżej 8. roku życia znajdują się pod stałą opieką osoby dorosłej. Aby dostać się na peron i usiąść na krzesełku należy przejść dwa rodzaje bramek:
- wejście na teren dolnej stacji odbywa się czterema bramkami jednocześnie – na tych bramkach sprawdzany jest bilet/karnet,  
- wejście na peron odbywa się trzema bramkami – na tych bramkach następuje kontrola osób, które mają usiąść na jednym krzesełku; bramki te otwiera pracownik1.  

Na terenie dolnej stacji (po przejściu pierwszych bramek, a przed wyjściem drugimi bramkami) może przebywać maksymalnie N osób. Wyjazd/wyjście z peronu stacji górnej odbywa się dwoma drogami jednocześnie (ruch jednokierunkowy). Stacja dolna jest obsługiwana przez pracownika1, stacja górna jest obsługiwana przez pracownika2. W przypadku zagrożenia pracownik1 lub pracownik2 zatrzymują kolej linową (sygnał1). Aby wznowić działanie, pracownik który zatrzymał kolej komunikuje się z drugim pracownikiem – po otrzymaniu komunikatu zwrotnego o gotowości kolej jest uruchamiana ponownie (sygnał2).

Rowerzyści zjeżdżają trzema trasami o różnym stopniu trudności – średni czas przejazdu dla poszczególnych tras jest różny i wynosi odpowiednio T1, T2 i T3 (T1 < T2 < T3).

Zasady działania stacji ustalone przez kierownika są następujące:  
- Kolej linowa jest czynna w godzinach od Tp do Tk. W momencie osiągnięcia czasu Tk na bramkach przestają działać karnety. Wszystkie osoby, które weszły na peron mają zostać przetransportowane do stacji górnej. Następnie po 3 sekundach kolej ma zostać wyłączona.  
- Dzieci w wieku od 4 do 8 lat siadają na krzesełko pod opieką osoby dorosłej.  
- Osoba dorosła może opiekować się jednocześnie co najwyżej dwoma dziećmi w wieku od 4 do 8 lat.  
- Każde przejście przez bramki (użycie danego karnetu) jest rejestrowane (id karnetu – godzina). Na koniec dnia jest generowany raport/podsumowanie liczby wykonanych zjazdów przez poszczególne osoby/karnety.  
- Osoby uprawnione VIP (ok. 1%) wchodzą na teren dolnej stacji bez kolejki (używając karnetu!).  

Napisz procedury **Kasjer**, **Pracownik** i **Turysta** symulujące działanie kolei linowej. Raport z przebiegu symulacji należy zapisać w pliku (plikach) tekstowym.


## 2. Testy

### Test 1: Weryfikacja konfiguracji obsadzenia krzesełek
- **Cel:** Sprawdzenie, czy pracownik poprawnie grupuje osoby wchodzące na peron.
- **Dane wejściowe:** W kolejce na peron czekają w losowej kolejności: 5 
- **Oczekiwane zachowanie:**
    - Krzesełka nie wyjeżdżają z niedozwolonymi kombinacjami (np. 3 rowerzystów).
    - Możliwe obsadzenia w logach: [2 Rowerzystów], [4 Pieszych], [1 Rowerzysta + 2 Pieszych].
    - System nie pozwala wejść na krzesełko większej liczbie osób niż 4.

### Test 2: Przepełnienie i limit krzesełek
**Cel:** Sprawdzenie limitu **N** osób na stacji oraz limitu 36 zajętych krzesełek.
- **Dane wejściowe:** Bardzo duża liczba turystów (np. 200) pojawiająca się w krótkim czasie. Ustawienie N=10.
- **Oczekiwane zachowanie:**
    - Po wpuszczeniu 10 osób, 4 bramki wejściowe blokują się do momentu zwolnienia miejsca (wejścia ludzi na peron).
    - Jeżeli 36 krzesełek jest w trasie, kolejne nie są wypuszczane do momentu zwolnienia krzesełka na górnej stacji.
        
### Test 3: Procedura awaryjna (Zatrzymanie i Wznowienie)
**Cel:** Weryfikacja komunikacji między pracownikami i zatrzymania wszystkich procesów krzesełek.
- **Przebieg:**
    1.  Kolej działa normalnie.
    2.  Pracownik 2 losowo zgłasza zagrożenie (Sygnał 1).
    3.  Symulacja ruchu krzesełek zostaje wstrzymana (rowerzyści nie dojeżdżają na górę, nikt nie wsiada).
    4.  Pracownik 2 wysyła komunikat o gotowości.
    5.  Pracownik 1 potwierdza i wysyła Sygnał 2.
    6.  Ruch zostaje wznowiony.
        

### Test 4: Opieka nad dziećmi i priorytet VIP
**Cel:** obsługa kolejki priorytetowej I sprawdzenie logiki "opieki".
-   **Dane wejściowe:**
    -   Grupa A: 3 dzieci (<8 lat) + 1 dorosły.
    -   Grupa B: VIP.
    -   Grupa C: Zwykli turyści.
-   **Oczekiwane zachowanie:**
    -   VIP wchodzi na stację przed osobami czekającymi w normalnej kolejce.
    -   Grupa A zostaje rozdzielona lub zablokowana: 1 dorosły może zabrać tylko 2 dzieci. Trzecie dziecko nie może wejść na krzesełko samo (musi czekać na innego dorosłego lub system zgłasza błąd opieki).
        

### Test 5: Zakończenie dnia pracy (Czas Tk)
**Cel:** Poprawne zamknięcie systemu.
-   **Przebieg:**
    1.  Symulacja osiąga czas Tk.
    2.  Bramki wejściowe (4 szt.) przestają przyjmować nowe karnety.
    3.  Osoby będące w strefie buforowej są wpuszczane na peron i wywożone na górę.
    4.  Po obsłużeniu ostatniej osoby z peronu system czeka 3 sekundy.
    5.  Generowany jest plik tekstowy z raportem (lista ID karnetów i liczba zjazdów).
    6.  Program kończy działanie.
