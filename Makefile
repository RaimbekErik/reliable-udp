CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2
COMMON  = crc32.o packet.o util.o

all: sender receiver test_crc

sender: sender.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^

receiver: receiver.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^

test_crc: test_crc.o crc32.o packet.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c common.h
	$(CC) $(CFLAGS) -c $<

test: all
	./test_crc
	bash tests/run_tests.sh

clean:
	rm -f *.o sender receiver test_crc
	rm -rf tests/out

.PHONY: all test clean
