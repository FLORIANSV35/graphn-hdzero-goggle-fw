CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter

.PHONY: all run clean

all: test_avshare_reset test_rolling_subfolders

test_avshare_reset: test_avshare_reset.c
	$(CC) $(CFLAGS) $< -o $@

test_rolling_subfolders: test_rolling_subfolders.c ../../src/record/rolling.c
	$(CC) $(CFLAGS) -I../../src/record $^ -o $@

run: test_avshare_reset test_rolling_subfolders
	./test_avshare_reset
	./test_rolling_subfolders

clean:
	rm -f test_avshare_reset test_rolling_subfolders
