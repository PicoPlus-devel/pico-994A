# Host tests for the cassette port. Builds the real ti99/cassette.c and
# ti99/cpu/tms9900/tms9901.c against stubs, so the tests drive the same code the
# firmware runs. Run with: make -C tools/tapetest run
TI  := ../../ti99
SRC := harness.c tests.c $(TI)/cassette.c $(TI)/cpu/tms9900/tms9901.c
CFLAGS := -g -O1 -Wall -Wno-format-truncation -I. -I$(TI) -I$(TI)/cpu/tms9900

tapetest: $(SRC) ff.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

run: tapetest
	./tapetest

clean:
	rm -rf tapetest sandbox

.PHONY: run clean
