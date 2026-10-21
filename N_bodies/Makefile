CC = gcc
CFLAGS = -O2 -std=c11 -Wall -Wextra -Wpedantic
LDLIBS = -lm

all: nbody_serial nbody_pthreads

nbody_serial: nbody_serial.c nbody.c nbody.h timer.h
	$(CC) $(CFLAGS) nbody_serial.c nbody.c -o $@ $(LDLIBS)

nbody_pthreads: nbody_pthreads.c nbody.c nbody.h timer.h
	$(CC) $(CFLAGS) -pthread nbody_pthreads.c nbody.c -o $@ $(LDLIBS)

clean:
	rm -f nbody_serial nbody_pthreads

.PHONY: all clean
