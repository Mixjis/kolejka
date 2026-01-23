CC=gcc
CFLAGS=-Wall -std=c11 -lrt
PROGRAM=kolej_linowa
LOGI=kolej_log.txt

kolej_linowa: main.c
	$(CC) $(CFLAGS) -o $@ $<
clean:
	rm -f $(PROGRAM) $(LOGI)
run:
	./$(PROGRAM)
#test: