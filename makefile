CFLAGS += -Wall -Wextra -Wpedantic -Waggregate-return
CFLAGS += -Wvla -Wwrite-strings -Wfloat-equal
CFLAGS += -std=c18


ashti:

.PHONY: clean
clean:
	$(RM) ashti *.o

.PHONY: debug
debug: CFLAGS += -g
debug: ashti

.PHONY: profile
profile: CFLAGS += -pg
profile: LDFLAGS += -pg
profile: ashti

.PHONY: check
check: ashti
check:
	./test/test.bash

