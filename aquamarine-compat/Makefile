CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -fPIC -Wall -Wextra -Werror
LDFLAGS ?= -shared
LDLIBS ?= -ldl

PROBE_OUT := dist/linux/aarch64/libdrm-lease-probe.so
COMPAT_OUT := dist/linux/aarch64/libdrm-lease-compat.so
PRIME_OUT := dist/linux/aarch64/libdrm-prime-compat.so
NAME_OUT := dist/linux/aarch64/libdrm-device-name-compat.so

.PHONY: all probe compat prime name clean

all: probe compat prime name

probe: $(PROBE_OUT)

compat: $(COMPAT_OUT)

prime: $(PRIME_OUT)

name: $(NAME_OUT)

$(PROBE_OUT): src/drm_lease_probe.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(COMPAT_OUT): src/drm_lease_compat.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(PRIME_OUT): src/drm_prime_compat.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(NAME_OUT): src/drm_device_name_compat.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(PROBE_OUT) $(COMPAT_OUT) $(PRIME_OUT) $(NAME_OUT)
