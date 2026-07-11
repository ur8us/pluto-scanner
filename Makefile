CC = gcc

CFLAGS ?= -O2
WARN_CFLAGS ?= -Wall -Wextra -Wformat-security
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -liio -lm -pthread
THREAD_CFLAGS = -pthread
STD_CFLAGS = -std=c99

TARGET = pluto-scanner
TEST_TARGET = pluto-scanner-cic-test

.PHONY: all clean run check smoke-test cic-synthetic-test

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(STD_CFLAGS) $(WARN_CFLAGS) $(CFLAGS) $(THREAD_CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

$(TEST_TARGET): main.c
	$(CC) $(STD_CFLAGS) $(WARN_CFLAGS) -Wno-unused-function $(CFLAGS) $(THREAD_CFLAGS) $(CPPFLAGS) -DPSEUDO_RANDOM_SAMPLE_SOURCE=2 -o $@ $< $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	PLUTO_URI="$${PLUTO_URI:-ip:192.168.2.1}" ./$(TARGET)

smoke-test:
	tools/http_smoke_test.sh

check: all $(TEST_TARGET)
	perl -0777 -ne 'print $$1 if /<script>(.*?)<\/script>/s' index.html | node --check
	tools/cic_stability_check.py --quiet
	tools/cic_continuity_check.py --quiet
	tools/cic_synthetic_signal_check.py --quiet
	@echo "Build checks passed."

cic-synthetic-test: $(TEST_TARGET)
	tools/cic_synthetic_signal_check.py

clean:
	rm -f $(TARGET) $(TEST_TARGET)
