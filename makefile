CC=gcc
CFLAGS=-Wall -std=c11
PROGRAM=kolej_linowa

kolej_linowa: main.c
	$(CC) $(CFLAGS) -o $@ $<
clean:
	rm -f $(PROGRAM)
run:
	./$(PROGRAM)
#test: