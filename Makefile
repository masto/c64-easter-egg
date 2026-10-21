CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
BUILD   := build

MODEL   := src/c64float.c src/c64rnd.c
ORACLE  := test/cpu6502.c test/c64machine.c

FASTFLAGS ?= -O3 -mcpu=native -Wall -Wextra -std=gnu11

all: $(BUILD)/seedsearch $(BUILD)/runbasic $(BUILD)/test_rnd $(BUILD)/fastsearch $(BUILD)/test_fast $(BUILD)/eggmaker \
     $(if $(filter Darwin,$(shell uname -s)),$(BUILD)/gpusearch)

$(BUILD)/fastsearch: src/fast/fastsearch.c src/fast/*.h | $(BUILD)
	$(CC) $(FASTFLAGS) -o $@ src/fast/fastsearch.c -lpthread

$(BUILD)/eggmaker: src/fast/eggmaker.c src/fast/rndfast.h $(MODEL) src/*.h | $(BUILD)
	$(CC) $(FASTFLAGS) -o $@ src/fast/eggmaker.c $(MODEL) -lpthread

$(BUILD)/test_fast: test/test_fast.c src/fast/*.h $(MODEL) src/*.h | $(BUILD)
	$(CC) $(FASTFLAGS) -o $@ test/test_fast.c $(MODEL)

# The Metal kernel is embedded as source and compiled when gpusearch starts.
$(BUILD)/search_metal.h: src/fast/search.metal | $(BUILD)
	cd src/fast && xxd -i search.metal > ../../$@

$(BUILD)/gpusearch: src/fast/gpusearch.m $(BUILD)/search_metal.h src/fast/rndfast.h | $(BUILD)
	$(CC) -O2 -Wall -fobjc-arc -I$(BUILD) -Isrc/fast -o $@ src/fast/gpusearch.m -framework Metal -framework Foundation

$(BUILD)/seedsearch: src/seedsearch.c $(MODEL) src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/seedsearch.c $(MODEL)

$(BUILD)/runbasic: test/runbasic.c $(ORACLE) test/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/runbasic.c $(ORACLE)

$(BUILD)/test_rnd: test/test_rnd.c $(MODEL) $(ORACLE) src/*.h test/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/test_rnd.c $(MODEL) $(ORACLE)

$(BUILD)/%.prg: basic/%.bas | $(BUILD)
	petcat -w2 -o $@ -- $<

$(BUILD):
	mkdir -p $@

# Quick checks (about a minute).
test: all $(BUILD)/easteregg.prg
	$(BUILD)/test_rnd 20000 10 200000
	$(BUILD)/test_fast 100000 2000000 200000000
	test/compare_fast.sh
	test/test_eggmaker.sh
	@$(BUILD)/runbasic $(BUILD)/easteregg.prg 2>/dev/null | tr -s '\n' ' ' | grep -q 'BILL GATES SUCKS' \
	    && echo "OK   easter egg prints BILL GATES SUCKS on the ROMs"
	@test "$$($(BUILD)/seedsearch BILL 125708 125708)" = "125708 BILL" \
	    && test "$$($(BUILD)/seedsearch GATES 33435700 33435700)" = "33435700 GATES" \
	    && test "$$($(BUILD)/seedsearch SUCKS 17059266 17059266)" = "17059266 SUCKS" \
	    && echo "OK   seedsearch finds the easter egg's seeds"
	test/compare_basic.sh robin E 1 20000
	test/compare_basic.sh egg E 1 20000

# Minutes: millions of seeds against the ROM, plus longer BASIC runs.
test-long: all
	$(BUILD)/test_rnd 1000000 10 10000000
	$(BUILD)/test_fast
	test/compare_basic.sh robin AN 1 200000
	test/compare_basic.sh egg AN 1 200000
	test/compare_basic.sh egg THE 1 300000

# Checks the oracle itself against VICE.
test-vice: all
	test/vice_crosscheck.sh

clean:
	rm -rf $(BUILD)

.PHONY: all test test-long test-vice clean
