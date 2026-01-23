CC=gcc
CFLAGS=-Wall -Wextra -std=gnu11 -pthread
LDFLAGS=-pthread -lrt
TARGETS=main logger cashier worker worker2 tourist

all: $(TARGETS)

main: main.c utils.o common.h
	$(CC) $(CFLAGS) -o $@ main.c utils.o $(LDFLAGS)

logger: logger.c utils.o common.h
	$(CC) $(CFLAGS) -o $@ logger.c utils.o $(LDFLAGS)

cashier: cashier.c utils.o common.h
	$(CC) $(CFLAGS) -o $@ cashier.c utils.o $(LDFLAGS)

worker: worker.c utils.o common.h
	$(CC) $(CFLAGS) -o $@ worker.c utils.o $(LDFLAGS)

worker2: worker2.c utils.o common.h
	$(CC) $(CFLAGS) -o $@ worker2.c utils.o $(LDFLAGS)

tourist: tourist.c utils.o common.h
	$(CC) $(CFLAGS) -o $@ tourist.c utils.o $(LDFLAGS)

utils.o: utils.c common.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGETS) *.o kolej_log.txt rides_log.txt /tmp/kolej_worker_fifo

run: all
	./main

.PHONY: all clean run
