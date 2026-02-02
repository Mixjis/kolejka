# Makefile dla symulacji kolei linowej
# Kompilacja: make
# Uruchomienie: make run
# Czyszczenie: make clean

CC = gcc
CFLAGS = -Wall -Wextra -pthread -D_GNU_SOURCE -g
LDFLAGS = -pthread

# Katalog źródłowy
SRCDIR = src

# Pliki źródłowe i docelowe
SOURCES = $(SRCDIR)/main.c $(SRCDIR)/cashier.c $(SRCDIR)/worker.c $(SRCDIR)/worker2.c $(SRCDIR)/tourist.c $(SRCDIR)/logger.c $(SRCDIR)/utils.c
HEADERS = $(SRCDIR)/struktury.h $(SRCDIR)/utils.h $(SRCDIR)/logger.h

# Główne pliki wykonywalne
MAIN = kolej
CASHIER = cashier
WORKER = worker
WORKER2 = worker2
TOURIST = tourist

# Pliki obiektowe wspólne
COMMON_OBJ = utils.o logger.o

all: $(MAIN) $(CASHIER) $(WORKER) $(WORKER2) $(TOURIST)

# Główny program
$(MAIN): main.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

# Kasjer
$(CASHIER): cashier.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

# Pracownik 1 (stacja dolna)
$(WORKER): worker.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

# Pracownik 2 (stacja górna)
$(WORKER2): worker2.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

# Turysta
$(TOURIST): tourist.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

# Kompilacja plików obiektowych z katalogu src
%.o: $(SRCDIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

# Uruchomienie symulacji
run: all
	./$(MAIN)

# Czyszczenie
clean:
	rm -f *.o $(MAIN) $(CASHIER) $(WORKER) $(WORKER2) $(TOURIST)
	rm -f kolej_log.txt raport_karnetow.txt

# Pomoc
help:
	@echo "Dostępne cele:"
	@echo "  make        	- kompilacja wszystkich plików"
	@echo "  make run    	- kompilacja i uruchomienie symulacji"
	@echo "  make clean  	- usunięcie plików wykonywalnych i obiektowych"
	@echo "  make help   	- ta pomoc"

.PHONY: all run clean help
