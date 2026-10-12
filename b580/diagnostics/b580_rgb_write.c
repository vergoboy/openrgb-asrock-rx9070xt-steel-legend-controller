/*
 * b580_rgb_write.c
 *
 * Smallest standalone reproduction of the confirmed B580 RGB control path:
 *
 *   OpenRGB -> /dev/i2c-28 (Synopsys DesignWare) -> raw 12-byte I2C block
 *   write to address 0x36 (the GPU AMC) ->
 *     [0x10, 0x00, channel, mode, R, G, B, speed, brightness, direction, 0x1A, 0x00]
 *   -> B580 RGB changed (user-confirmed ground truth).
 *
 * This is intentionally identical to OpenRGB's i2c_smbus_linux::i2c_xfer()
 * (a single struct i2c_msg write via the I2C_RDWR ioctl), so a successful
 * colour change here proves the standalone transport and lets us validate
 * the OpenRGB controller path without building the whole of OpenRGB.
 *
 * Build:   make b580_rgb_write
 * Run:     sudo ./b580_rgb_write [bus] [addr] [channel] [mode] [rgb]
 *
 * Examples:
 *   sudo ./b580_rgb_write                      # default: green, mode 0x01 Static
 *   sudo ./b580_rgb_write 28 0x36 0x03 0x01 ff0000   # red
 *   sudo ./b580_rgb_write 28 0x36 0x03 0x01 0000ff   # blue
 *   sudo ./b580_rgb_write 28 0x36 0x03 0x00 000000   # off (mode 0x00)
 *
 * Device permission: /dev/i2c-28 is root:root (or i2c group). Run with sudo
 * or add your user to the i2c group.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int write_block(int fd, uint8_t addr, const uint8_t* data, int len)
{
    struct i2c_rdwr_ioctl_data rdwr;
    struct i2c_msg              msg;

    msg.addr  = addr;
    msg.flags = 0;                 /* I2C_M_WRITE (0) */
    msg.len   = (uint16_t)len;
    msg.buf   = (uint8_t*)data;

    rdwr.msgs  = &msg;
    rdwr.nmsgs = 1;

    return ioctl(fd, I2C_RDWR, &rdwr);
}

int main(int argc, char** argv)
{
    int      bus        = 28;
    uint8_t  addr       = 0x36;
    uint8_t  channel    = 0x03;    /* "ARGB Header" */
    uint8_t  mode       = 0x01;    /* Static */
    uint8_t  red        = 0x00;
    uint8_t  green      = 0xFF;    /* default test colour: green */
    uint8_t  blue       = 0x00;
    uint8_t  speed      = 0x80;
    uint8_t  brightness = 0xFF;
    uint8_t  direction  = 0x00;

    if(argc >= 2) bus       = atoi(argv[1]);
    if(argc >= 3) addr      = (uint8_t)strtoul(argv[2], NULL, 0);
    if(argc >= 4) channel   = (uint8_t)strtoul(argv[3], NULL, 0);
    if(argc >= 5) mode      = (uint8_t)strtoul(argv[4], NULL, 0);
    if(argc >= 6)
    {
        /* "RRGGBB" hex, 6 chars */
        long rgb = strtol(argv[5], NULL, 16);
        red      = (uint8_t)((rgb >> 16) & 0xFF);
        green    = (uint8_t)((rgb >>  8) & 0xFF);
        blue     = (uint8_t)((rgb      ) & 0xFF);
    }

    uint8_t packet[12] =
    {
        0x10, 0x00, channel,
        mode, red, green, blue,
        speed, brightness, direction,
        0x1A, 0x00
    };

    char devpath[64];
    snprintf(devpath, sizeof(devpath), "/dev/i2c-%d", bus);

    int fd = open(devpath, O_RDWR);
    if(fd < 0)
    {
        fprintf(stderr, "open(%s): %s\n", devpath, strerror(errno));
        return 1;
    }

    printf("Writing to %s addr 0x%02X: channel=0x%02X mode=0x%02X RGB=%02X%02X%02X "
           "speed=0x%02X brightness=0x%02X direction=0x%02X\n",
           devpath, addr, channel, mode, red, green, blue, speed, brightness, direction);
    printf("packet: ");
    for(int i = 0; i < 12; i++)
    {
        printf("%02X ", packet[i]);
    }
    printf("\n");

    int ret = write_block(fd, addr, packet, sizeof(packet));
    if(ret < 0)
    {
        fprintf(stderr, "I2C_RDWR write failed: %s (ret=%d)\n", strerror(errno), ret);
        close(fd);
        return 1;
    }

    printf("write ok (ret=%d). Check the GPU RGB has changed.\n", ret);
    close(fd);
    return 0;
}