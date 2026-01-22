CC=gcc
CFLAGS=-Wall -std=gnu11 -pthread -lrt
MAIN_EXEC=kolej_linowa 
TARGETS=main logger cashier worker tourist

all: $(TARGETS)

main: main.c utils.o
	$(CC) $(CFLAGS) -o $@ $^

logger: logger.c utils.o
	$(CC) $(CFLAGS) -o $@ $^

cashier: cashier.c utils.o
	$(CC) $(CFLAGS) -o $@ $^

worker: worker.c utils.o
	$(CC) $(CFLAGS) -o $@ $^

tourist: tourist.c utils.o
	$(CC) $(CFLAGS) -o $@ $^

utils.o: utils.c common.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGETS) *.o kolej_log.txt

run: all
	./main

.PHONY: all clean run