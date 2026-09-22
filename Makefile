# GRS Syncword Detector.
#
# Two products from one source tree:
#
#   libsyncword.a     the detector itself, for whoever wants to link it
#   grs_syncword      the ZMQ service (service.c) that the station runs
#
# The service is a separate target from the library on purpose: linking the
# library into something else must never drag in libzmq.

TARGET_LIB = libsyncword.a
TARGET_SERVICE = grs_syncword
TARGET_SMOKE = syncword_smoke

CC = gcc

# The adopted detector carries pre-existing -Wsign-compare warnings, so it is
# built with -Wall -Wextra but WITHOUT -Werror; silencing them would mean
# touching the algorithm for cosmetics. Our own code is held to -Werror.
FLAGS = -std=c99 -Wall -Wextra

# -D_POSIX_C_SOURCE: strict -std=c99 hides the POSIX declarations, so
# sigaction() and gmtime_r() come through as implicit declarations and the
# build fails in a way that reads like the functions do not exist. Declaring
# the POSIX level keeps the C dialect strict and brings them back.
OUR_FLAGS = $(FLAGS) -Werror -D_POSIX_C_SOURCE=200809L

all: $(TARGET_LIB) $(TARGET_SERVICE) $(TARGET_SMOKE)

syncword.o: syncword.c syncword.h
	$(CC) $(FLAGS) -c syncword.c -o syncword.o

$(TARGET_LIB): syncword.o
	ar rcs $(TARGET_LIB) syncword.o

service.o: service.c syncword.h
	$(CC) $(OUR_FLAGS) -c service.c -o service.o

$(TARGET_SERVICE): service.o syncword.o
	$(CC) service.o syncword.o -o $(TARGET_SERVICE) -lzmq

test/syncword_smoke.o: test/syncword_smoke.c syncword.h
	$(CC) $(OUR_FLAGS) -I. -c test/syncword_smoke.c -o test/syncword_smoke.o

$(TARGET_SMOKE): test/syncword_smoke.o syncword.o
	$(CC) test/syncword_smoke.o syncword.o -o $(TARGET_SMOKE)

# Builds the library and proves it detects. Runs anywhere, no ZMQ, no radio.
check: $(TARGET_SMOKE)
	./$(TARGET_SMOKE)

install: $(TARGET_SERVICE)
	install -m 0755 $(TARGET_SERVICE) /usr/local/bin/$(TARGET_SERVICE)

clean:
	rm -f *.o test/*.o $(TARGET_LIB) $(TARGET_SERVICE) $(TARGET_SMOKE)

.PHONY: all check install clean
