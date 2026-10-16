CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined
APPCONF := ../../components/appconf

.PHONY: test clean

test: test_appconf test_history_ring
	./test_appconf
	./test_history_ring

test_appconf: test_appconf.c tinytest.h $(APPCONF)/appconf.c $(APPCONF)/include/appconf.h
	$(CC) $(CFLAGS) -I$(APPCONF)/include -o $@ test_appconf.c $(APPCONF)/appconf.c

test_history_ring: test_history_ring.c tinytest.h ../../main/history_ring.c ../../main/history_ring.h
	$(CC) $(CFLAGS) -I../../main -o $@ test_history_ring.c ../../main/history_ring.c

clean:
	rm -f test_appconf test_history_ring
