CFLAGS += -Wall -Wextra -Wpedantic -Waggregate-return
CFLAGS += -Wvla -Wwrite-strings -Wfloat-equal
CFLAGS += -std=c18


ashti: ashti.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

.PHONY: clean
clean:
	rm -f ashti *.o

.PHONY: debug
debug: CFLAGS += -g
debug: ashti

.PHONY: profile
profile: CFLAGS += -pg
profile: ashti

