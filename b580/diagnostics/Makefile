CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -std=gnu11

all: pcode_rgb_probe i2c_endpoint_probe b580_rgb_write pcode_rgb_write

pcode_rgb_probe: pcode_rgb_probe.c pcode_protocol.h
	$(CC) $(CFLAGS) -o $@ pcode_rgb_probe.c

i2c_endpoint_probe: i2c_endpoint_probe.c
	$(CC) $(CFLAGS) -o $@ i2c_endpoint_probe.c

b580_rgb_write: b580_rgb_write.c
	$(CC) $(CFLAGS) -o $@ b580_rgb_write.c

pcode_rgb_write: pcode_rgb_write.c
	$(CC) $(CFLAGS) -o $@ pcode_rgb_write.c

isp_i2c_probe: isp_i2c_probe.c
	$(CC) $(CFLAGS) -o $@ isp_i2c_probe.c

clean:
	rm -f pcode_rgb_probe i2c_endpoint_probe b580_rgb_write pcode_rgb_write isp_i2c_probe
