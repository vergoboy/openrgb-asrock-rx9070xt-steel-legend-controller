// SPDX-License-Identifier: GPL-2.0
/*
 * pcode_rgb_write.c - deterministic standalone B580 RGB command via PCODE.
 *
 * Confirmed (user-verified) RGB transport for the ASRock Arc B580 Steel
 * Legend GPU:
 *
 *   PCODE mailbox write, command 0x54, subcommand 0x00,
 *   DATA0 = (R << 16) | (G << 8) | B   ->  B580 RGB changes.
 *
 * Sending pure red (data 0x00FF0000) visibly turned the GPU LED red.
 *
 * This mirrors exactly what `pcode_rgb_probe --interactive`'s `DC` command
 * does, but as a small self-contained program so it is easy to script,
 * reproduce, and integrate.
 *
 * PCODE mailbox registers (GPU PCI BAR0):
 *   PCODE_MAILBOX = 0x138124   (bit31 BUSY/READY; bits[7:0] cmd;
 *                               bits[15:8] param1; bits[23:16] param2; bits[7:0] error on completion)
 *   PCODE_DATA0   = 0x138128
 *   PCODE_DATA1   = 0x13812C
 *
 * Build:   make pcode_rgb_write
 * Run:     sudo ./pcode_rgb_write <rrggbb> [zone]
 *
 * Examples:
 *   sudo ./pcode_rgb_write ff0000            # pure red   (matches DC ff 0 0)
 *   sudo ./pcode_rgb_write 00ff00            # green
 *   sudo ./pcode_rgb_write 0000ff            # blue
 *   sudo ./pcode_rgb_write 000000            # black (tentative off)
 *   sudo ./pcode_rgb_write ff0000 0x03       # red on zone 0x03 (DCZ)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>

#define BAR0_SIZE               (16 * 1024 * 1024)
#define PCODE_MAILBOX_OFFSET    0x138124
#define PCODE_DATA0_OFFSET      0x138128
#define PCODE_DATA1_OFFSET      0x13812C

#define PCODE_READY             (1u << 31)
#define PCODE_ERROR_MASK        0x000000FFu
#define PCODE_TIMEOUT_US        1000
#define PCODE_SUCCESS           0x0
#define PCODE_MBOX(mbcmd, p1, p2) \
    (((uint32_t)(mbcmd) & 0xFF) | (((uint32_t)(p1) & 0xFF) << 8) | (((uint32_t)(p2) & 0xFF) << 16))

#define CMD_LIGHTING            0x54
#define SUB_LIGHTING            0x00

#define B580_VENDOR_ID          0x8086
#define B580_DEVICE_ID          0xe20b

static volatile uint32_t *mmio;
static int                 mem_fd = -1;

static int find_b580_pci(char *out, size_t outlen)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    struct dirent *ent;
    if (!dir)
        return -1;
    while ((ent = readdir(dir)) != NULL) {
        char dp[512], vp[512], did[16], vid[16];
        FILE *fp;
        if (ent->d_name[0] == '.')
            continue;
        snprintf(dp, sizeof(dp), "/sys/bus/pci/devices/%s/device", ent->d_name);
        snprintf(vp, sizeof(vp), "/sys/bus/pci/devices/%s/vendor", ent->d_name);
        fp = fopen(dp, "r");
        if (!fp) continue;
        if (!fgets(did, sizeof(did), fp)) { fclose(fp); continue; }
        fclose(fp);
        fp = fopen(vp, "r");
        if (fp) { fgets(vid, sizeof(vid), fp); fclose(fp); }
        if (strtol(did, NULL, 16) == B580_DEVICE_ID &&
            (!vid[0] || strtol(vid, NULL, 16) == B580_VENDOR_ID)) {
            snprintf(out, outlen, "%s", ent->d_name);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

static inline uint32_t mmio_read(uint32_t off)
{
    return mmio[off / 4];
}
static inline void mmio_write(uint32_t off, uint32_t val)
{
    mmio[off / 4] = val;
}

static int wait_ready(uint32_t timeout_us)
{
    uint32_t elapsed = 0, step = 10;
    while (elapsed < timeout_us) {
        if ((mmio_read(PCODE_MAILBOX_OFFSET) & PCODE_READY) == 0)
            return 0;
        usleep(step);
        elapsed += step;
    }
    return -ETIMEDOUT;
}

/* pcode_write(0x54, 0x00, zone, data0, 0) -> RGB colour write. */
static int pcode_rgb_write(uint32_t cmd, uint32_t p1, uint32_t p2, uint32_t data0)
{
    int ret;
    if (mmio_read(PCODE_MAILBOX_OFFSET) & PCODE_READY) {
        ret = wait_ready(PCODE_TIMEOUT_US);
        if (ret) return ret;
    }
    mmio_write(PCODE_DATA0_OFFSET, data0);
    mmio_write(PCODE_DATA1_OFFSET, 0);
    mmio_write(PCODE_MAILBOX_OFFSET, PCODE_READY | PCODE_MBOX(cmd, p1, p2));
    ret = wait_ready(PCODE_TIMEOUT_US);
    if (ret) return ret;
    uint32_t status = mmio_read(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
    return (status == PCODE_SUCCESS) ? 0 : (int)status;
}

int main(int argc, char **argv)
{
    uint32_t rgb = 0x00FF0000;   /* default: pure red (the confirmed value) */
    uint32_t zone = 0x00;        /* DC uses param2=0; DCZ passes a zone     */
    char     pci_bdf[32] = "";

    if (argc >= 2)
        rgb = (uint32_t)strtoul(argv[1], NULL, 16) & 0x00FFFFFF;
    if (argc >= 3)
        zone = (uint32_t)strtoul(argv[2], NULL, 0) & 0xFF;

    if (find_b580_pci(pci_bdf, sizeof(pci_bdf)) < 0) {
        fprintf(stderr, "B580 (8086:e20b) not found; set XE_PCI_BDF\n");
        return 2;
    }

    char bar0[512];
    snprintf(bar0, sizeof(bar0), "/sys/bus/pci/devices/%s/resource0", pci_bdf);
    mem_fd = open(bar0, O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "open(%s): %s (need root?)\n", bar0, strerror(errno));
        return 2;
    }

    void *map = mmap(NULL, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        close(mem_fd);
        return 2;
    }
    mmio = (volatile uint32_t *)map;

    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >>  8) & 0xFF;
    uint32_t b = (rgb      ) & 0xFF;

    printf("PCODE RGB write: PCI=%s cmd=0x%02X sub=0x%02X zone=0x%02X "
           "DATA0=0x%06X (R=%02X G=%02X B=%02X)\n",
           pci_bdf, CMD_LIGHTING, SUB_LIGHTING, zone, rgb, r, g, b);

    int ret = pcode_rgb_write(CMD_LIGHTING, SUB_LIGHTING, zone, rgb);
    if (ret == 0) {
        printf("PCODE write OK. Check GPU RGB has changed.\n");
    } else {
        printf("PCODE write failed: ret=%d (status 0x%02X)\n",
               ret, mmio_read(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK);
    }

    munmap(map, BAR0_SIZE);
    close(mem_fd);
    return (ret == 0) ? 0 : 3;
}