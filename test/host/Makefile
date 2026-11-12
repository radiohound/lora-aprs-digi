# Host test of the digipeater core (path rule, positions, moving stations, station list)
CFLAGS  ?= -O2 -g -Wall -Wextra -fsanitize=address,undefined
SRC = test_digi3.c ../../src/digi3/digi3_core.c ../../src/digi3/neighbors.c

test: test_digi3
	./test_digi3

test_digi3: $(SRC) ../../src/digi3/digi3_core.h ../../src/digi3/neighbors.h
	$(CC) $(CFLAGS) -I../../src/digi3 -o $@ $(SRC) -lm

clean:
	rm -f test_digi3 test_settings test_remote test_battery

.PHONY: test clean

# the settings core alone (no codec needed)
test-settings: test_settings
	./test_settings

test_settings: test_settings.c ../../src/digi3/settings.c ../../src/digi3/settings.h ../../src/digi3/config_digi3.h
	$(CC) $(CFLAGS) -I../../src/digi3 -o $@ test_settings.c ../../src/digi3/settings.c

.PHONY: test-settings

# settings over the air (and the code tool, when python3 is there)
test-remote: test_remote
	./test_remote

test_remote: test_remote.c ../../src/digi3/remote.c ../../src/digi3/remote.h ../../src/digi3/settings.c ../../src/digi3/settings.h
	$(CC) $(CFLAGS) -I../../src/digi3 -o $@ test_remote.c ../../src/digi3/remote.c ../../src/digi3/settings.c

.PHONY: test-remote

# battery states
test-battery: test_battery
	./test_battery

test_battery: test_battery.c ../../src/digi3/battery.c ../../src/digi3/battery.h
	$(CC) $(CFLAGS) -I../../src/digi3 -o $@ test_battery.c ../../src/digi3/battery.c

.PHONY: test-battery
