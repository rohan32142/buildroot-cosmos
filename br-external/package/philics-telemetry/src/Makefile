CC ?= gcc
CFLAGS ?= -Wall -O2

philics-telemetry: main.o
	$(CC) $(CFLAGS) -o philics-telemetry main.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o philics-telemetry