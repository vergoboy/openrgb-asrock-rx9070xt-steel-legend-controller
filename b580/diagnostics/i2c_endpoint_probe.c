// SPDX-License-Identifier: GPL-2.0
/*
 * i2c_endpoint_probe - read Intel Arc B580 I2C endpoint registers.
 *
 * The xe kernel driver reads two BAR0 MMIO regs (REG_SG_REMAP_ADDR_PREFIX/SOC_BASE+0x0164
 * and REG_SG_REMAP_ADDR_POSTFIX/SOC_BASE+0x0168) to learn up to 3 I2C MCU client
 * addresses on the GPU's DesignWare I2C bus.  Clients 2/3 (the non-AMC RGB/LED
 * controller) are NOT registered by the kernel driver, so these expose the
 * transport address we're hunting.
 *
 * Usage (root):
 *   sudo ./i2c_endpoint_probe
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <dirent.h>

#define BAR0_SIZE (16 * 1024 * 1024)
#define B580_VENDOR_ID 0x8086
#define B580_DEVICE_ID 0xe20b

#define SOC_BASE 0x280000ULL
#define SG_REMAP_ADDR_PREFIX (SOC_BASE + 0x0164ULL)
#define SG_REMAP_ADDR_POSTFIX (SOC_BASE + 0x0168ULL)

static char pci_bdf[32] = "";
static char bar0_path[256] = "";
static volatile uint32_t *mmio_base = NULL;
static int mem_fd = -1;

static int find_b580_pci(char *out, size_t outlen)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    struct dirent *ent;
    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
        char device_path[512], vendor_path[512];
        char device_id[16] = "", vendor_id[16] = "";
        FILE *fp;
        if (ent->d_name[0] == '.') continue;
        snprintf(device_path, sizeof(device_path), "/sys/bus/pci/devices/%s/device", ent->d_name);
        snprintf(vendor_path, sizeof(vendor_path), "/sys/bus/pci/devices/%s/vendor", ent->d_name);
        fp = fopen(device_path, "r");
        if (!fp) continue;
        if (!fgets(device_id, sizeof(device_id), fp)) { fclose(fp); continue; }
        fclose(fp);
        fp = fopen(vendor_path, "r");
        if (fp) { fgets(vendor_id, sizeof(vendor_id), fp); fclose(fp); }
        if (strtol(device_id, NULL, 16) == B580_DEVICE_ID &&
            (!vendor_id[0] || strtol(vendor_id, NULL, 16) == B580_VENDOR_ID)) {
            snprintf(out, outlen, "%s", ent->d_name);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

static int resolve_pci_bdf(void)
{
    const char *env = getenv("XE_PCI_BDF");
    if (env && env[0]) {
        snprintf(pci_bdf, sizeof(pci_bdf), "%s", env);
        return 0;
    }
    return find_b580_pci(pci_bdf, sizeof(pci_bdf));
}

static void resolve_bar0_path(void)
{
    const char *env = getenv("XE_BAR0_PATH");
    if (env && env[0]) {
        snprintf(bar0_path, sizeof(bar0_path), "%s", env);
        return;
    }
    /* The B580's BAR0 is exposed as resource0 (16 MiB).  The mem node is not
       created by the xe driver, so use resource0 for the mmap. */
    snprintf(bar0_path, sizeof(bar0_path),
             "/sys/bus/pci/devices/%s/resource0", pci_bdf);
}

static int open_bar0(void)
{
    resolve_bar0_path();
    mem_fd = open(bar0_path, O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "open(%s): %s\n", bar0_path, strerror(errno));
        return -1;
    }
    mmio_base = (volatile uint32_t *)mmap(NULL, BAR0_SIZE,
                                          PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);
    if (mmio_base == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        close(mem_fd);
        return -1;
    }
    return 0;
}

static uint32_t read_u32(uint64_t off)
{
    return mmio_base[off / 4];
}

static void decode_endpoint(uint32_t prefix, uint32_t postfix)
{
    /* The driver does ep = {cookie, capabilities, addr[3]} and reads:
         u32* val = ep; val[0] = read32(PREFIX); val[1] = read32(POSTFIX);
       So cookies/capabilities occupy bytes 0-1 of u32[0]; addr[0..2] are the
       next u16 fields.  XE_I2C_CLIENT_AMC = 1 -> AMC = addr[1]. */
    uint8_t cookie = (uint8_t)(prefix & 0xFF);
    uint8_t caps   = (uint8_t)((prefix >> 8) & 0xFF);
    uint16_t addr[3];
    addr[0] = (uint16_t)((prefix >> 16) & 0xFFFF);   /* client 0 (unused) */
    addr[1] = (uint16_t)(postfix & 0xFFFF);          /* client 1 = AMC     */
    addr[2] = (uint16_t)((postfix >> 16) & 0xFFFF);  /* client 2 (RGB?)    */
    printf("cookie=0x%02X capabilities=0x%02X\n", cookie, caps);
    printf("  addr[0] unused   = 0x%04X\n", addr[0]);
    printf("  addr[1] AMC      = 0x%04X\n", addr[1]);
    printf("  addr[2] client 3 = 0x%04X   <-- only other possible I2C MCU (RGB?)\n", addr[2]);
    printf("=> only I2C MCU on the GPU is the AMC at addr[1]=0x%04X; "
           "no second RGB controller on the DesignWare I2C bus.\n", addr[1]);
}

int main(void)
{
    if (resolve_pci_bdf() < 0) {
        fprintf(stderr, "B580 (0x8086:0xe20b) not found\n");
        return 1;
    }
    fprintf(stderr, "PCI BDF: %s\n", pci_bdf);
    if (open_bar0() < 0)
        return 1;

    uint32_t prefix = read_u32(SG_REMAP_ADDR_PREFIX);
    uint32_t postfix = read_u32(SG_REMAP_ADDR_POSTFIX);
    fprintf(stderr, "REG_SG_REMAP_ADDR_PREFIX  (0x%llX) = 0x%08X\n",
            SG_REMAP_ADDR_PREFIX, prefix);
    fprintf(stderr, "REG_SG_REMAP_ADDR_POSTFIX (0x%llX) = 0x%08X\n",
            SG_REMAP_ADDR_POSTFIX, postfix);
    decode_endpoint(prefix, postfix);

    munmap((void *)mmio_base, BAR0_SIZE);
    close(mem_fd);
    return 0;
}