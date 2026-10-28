CC ?= cc
CFLAGS ?= -O2 -fPIC -Wall -Wextra
LDFLAGS ?=

all: libgbm-shim.so tests/gbm-probe

libgbm-shim.so: gbm_shim.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS)

tests/gbm-probe: tests/gbm-probe.c
	$(CC) $(CFLAGS) -o $@ $< -lgbm $(LDFLAGS)

clean:
	rm -f libgbm-shim.so tests/gbm-probe

.PHONY: all clean
