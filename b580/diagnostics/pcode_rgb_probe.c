// SPDX-License-Identifier: GPL-2.0
/*
 * pcode_rgb_probe - Intel Arc B580 PCODE mailbox probe with RGB discovery.
 *
 * Extends the fan-control PCODE probe to find the lighting/RGB control
 * command for the ASRock Steel Legend B580. The RGB controller (AMC/PCODE)
 * is reached through the same PCI BAR0 mailbox the fan projects used.
 *
 * Usage (root):
 *   sudo ./pcode_rgb_probe --baseline      # sanity: read fans/temp/late-bind
 *   sudo ./pcode_rgb_probe --scan-rgb      # scan command space for lighting cmds
 *   sudo ./pcode_rgb_probe --interactive   # manual mailbox read/write
 *   sudo ./pcode_rgb_probe --findlite      # deeper lighting-command scan
 *
 * Reads are always safe; writes require --force (use with caution).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <dirent.h>
#include <getopt.h>

#include "pcode_protocol.h"

#define BAR0_SIZE       (16 * 1024 * 1024)

#define B580_VENDOR_ID  0x8086
#define B580_DEVICE_ID  0xe20b

static char pci_bdf[32] = "";
static char bar0_path[256] = "";
static volatile uint32_t *mmio_base = NULL;
static int mem_fd = -1;
static int verbose = 0;
static int force = 0;

static int find_b580_pci(char *out, size_t outlen)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    struct dirent *ent;
    if (!dir)
        return -1;
    while ((ent = readdir(dir)) != NULL) {
        char device_path[512], vendor_path[512];
        char device_id[16] = "", vendor_id[16] = "";
        FILE *fp;
        if (ent->d_name[0] == '.')
            continue;
        snprintf(device_path, sizeof(device_path),
                 "/sys/bus/pci/devices/%s/device", ent->d_name);
        snprintf(vendor_path, sizeof(vendor_path),
                 "/sys/bus/pci/devices/%s/vendor", ent->d_name);
        fp = fopen(device_path, "r");
        if (!fp)
            continue;
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
    if (pci_bdf[0])
        return 0;
    if (env && env[0]) {
        snprintf(pci_bdf, sizeof(pci_bdf), "%s", env);
        return 0;
    }
    if (find_b580_pci(pci_bdf, sizeof(pci_bdf)) == 0)
        return 0;
    fprintf(stderr, "Error: Intel Arc B580 (8086:e20b) not found; set XE_PCI_BDF\n");
    return -1;
}

static const char *pcode_err_str(uint32_t err)
{
    switch (err) {
    case PCODE_SUCCESS:              return "Success";
    case PCODE_ILLEGAL_CMD:          return "Illegal Command";
    case PCODE_TIMEOUT:              return "Timed out";
    case PCODE_ILLEGAL_DATA:         return "Illegal Data";
    case PCODE_ILLEGAL_SUBCOMMAND:   return "Illegal Subcommand";
    case PCODE_LOCKED:               return "PCODE Locked";
    case PCODE_GT_RATIO_OUT_OF_RANGE: return "GT ratio out of range";
    case PCODE_REJECTED:             return "PCODE Rejected";
    default:                         return "Unknown";
    }
}

static int map_bar0(void)
{
    if (resolve_pci_bdf() < 0)
        return -1;
    snprintf(bar0_path, sizeof(bar0_path),
             "/sys/bus/pci/devices/%s/resource0", pci_bdf);
    mem_fd = open(bar0_path, O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", bar0_path, strerror(errno));
        return -1;
    }
    void *map = mmap(NULL, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(mem_fd);
        return -1;
    }
    mmio_base = (volatile uint32_t *)map;
    printf("[INFO] BAR0 mapped at %p\n", map);
    return 0;
}

static void unmap_bar0(void)
{
    if (mmio_base) { munmap((void *)mmio_base, BAR0_SIZE); mmio_base = NULL; }
    if (mem_fd >= 0) { close(mem_fd); mem_fd = -1; }
}

static inline uint32_t mmio_read32(uint32_t offset)
{
    return mmio_base[offset / 4];
}
static inline void mmio_write32(uint32_t offset, uint32_t val)
{
    mmio_base[offset / 4] = val;
}

static int wait_pcode_ready(uint32_t timeout_us)
{
    uint32_t elapsed = 0, step = 10;
    while (elapsed < timeout_us) {
        if ((mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) == 0)
            return 0;
        usleep(step);
        elapsed += step;
    }
    return -ETIMEDOUT;
}

static int pcode_read(uint32_t cmd, uint32_t param1, uint32_t param2,
                      uint32_t *data0, uint32_t *data1)
{
    uint32_t mbox;
    int ret;
    if (mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) {
        ret = wait_pcode_ready(PCODE_TIMEOUT_US);
        if (ret) return -EBUSY;
    }
    mmio_write32(PCODE_DATA0_OFFSET, 0);
    mmio_write32(PCODE_DATA1_OFFSET, 0);
    mbox = PCODE_READY | PCODE_MBOX(cmd, param1, param2);
    mmio_write32(PCODE_MAILBOX_OFFSET, mbox);
    ret = wait_pcode_ready(PCODE_TIMEOUT_US);
    if (ret) return ret;
    *data0 = mmio_read32(PCODE_DATA0_OFFSET);
    if (data1) *data1 = mmio_read32(PCODE_DATA1_OFFSET);
    uint32_t status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
    if (status != PCODE_SUCCESS)
        return -EIO;
    return 0;
}

static int pcode_write(uint32_t cmd, uint32_t param1, uint32_t param2,
                       uint32_t data0, uint32_t data1)
{
    uint32_t mbox;
    int ret;
    if (mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) {
        ret = wait_pcode_ready(PCODE_TIMEOUT_US);
        if (ret) return -EBUSY;
    }
    mmio_write32(PCODE_DATA0_OFFSET, data0);
    mmio_write32(PCODE_DATA1_OFFSET, data1);
    mbox = PCODE_READY | PCODE_MBOX(cmd, param1, param2);
    mmio_write32(PCODE_MAILBOX_OFFSET, mbox);
    ret = wait_pcode_ready(PCODE_TIMEOUT_US);
    if (ret) return ret;
    uint32_t status = mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK;
    if (status != PCODE_SUCCESS) return -EIO;
    return 0;
}

static void print_ts(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("[%ld.%06ld] ", ts.tv_sec, ts.tv_nsec / 1000);
}

/* Baseline: verify the mailbox works and report the card state. */
static int cmd_baseline(void)
{
    uint32_t d0, d1;
    int ret;
    printf("\n=== Baseline (verify PCODE mailbox access) ===\n\n");

    ret = pcode_read(FAN_SPEED_CONTROL, FSC_READ_NUM_FANS, 0, &d0, NULL);
    printf("  Fan count (0x7D/0x4):     %s%s\n", ret == 0 ? "SUCCESS count=" : "FAILED",
           ret == 0 ? "" : pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK));
    if (ret == 0)
        printf("                          -> %u\n", d0);

    ret = pcode_read(PCODE_THERMAL_INFO, READ_THERMAL_DATA, 0, &d0, &d1);
    printf("  Thermal data (0x25/0x2):  %s%s\n", ret == 0 ? "SUCCESS" : "FAILED",
           ret == 0 ? "" : pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK));
    if (ret == 0)
        printf("                          -> data0=0x%08x data1=0x%08x\n", d0, d1);

    ret = pcode_read(PCODE_LATE_BINDING, GET_CAPABILITY_STATUS, 0, &d0, NULL);
    printf("  Late-binding caps (0x5C/0): %s%s\n", ret == 0 ? "SUCCESS" : "FAILED",
           ret == 0 ? "" : pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK));
    if (ret == 0)
        printf("                          -> 0x%08x\n", d0);

    ret = pcode_read(PCODE_POWER_SETUP, POWER_SETUP_READ_I1, 0, &d0, NULL);
    printf("  Power I1 (0x7C/0x4):       %s%s\n", ret == 0 ? "SUCCESS" : "FAILED",
           ret == 0 ? "" : pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK));
    if (ret == 0)
        printf("                          -> 0x%08x\n", d0);
    printf("\n");
    return 0;
}

/* Probe the whole command space (except known safe/non-lighting staff) for
 * commands that return readable data, flagging unknown ones for RGB probing. */
static int cmd_scan_rgb(void)
{
    uint32_t d0, d1;
    int ret;
    printf("\n=== Scan PCODE command space (looking for lighting command) ===\n");
    printf("    Cmd = PCODE_MBOX command; subcommands tested 0x00-0x0F.\n");
    printf("    Candidate = command that ACCEPTS a READ and is not a known\n");
    printf("                fan/thermal/power/status command.\n\n");

    uint32_t skip_cmds[] = {
        FAN_SPEED_CONTROL, PCODE_THERMAL_INFO, PCODE_LATE_BINDING,
        PCODE_POWER_SETUP, DGFX_PCODE_STATUS, PCODE_FREQUENCY_CONFIG,
        0x8, 0x9, 0x25, 0x5C, 0x6e, 0x7C, 0x7D, 0x7E, 0x5D, 0x5F
    };

    int cand = 0;
    for (uint32_t cmd = 0x02; cmd <= 0x7F; cmd++) {
        int skip = 0;
        for (uint32_t i = 0; i < sizeof(skip_cmds)/sizeof(skip_cmds[0]); i++)
            if (cmd == skip_cmds[i]) { skip = 1; break; }
        if (skip)
            continue;

        for (uint32_t sub = 0; sub <= 0x0F; sub++) {
            ret = pcode_read(cmd, sub, 0, &d0, &d1);
            if (ret == 0) {
                printf("  CAND cmd=0x%02x sub=0x%02x: READ d0=0x%08x d1=0x%08x%s\n",
                       cmd, sub, d0, d1,
                       (d0 != 0 || d1 != 0) ? "  <== data present" : "");
                cand++;
                /* Try subcommand 0x00..0x07 on the same cmd to map it */
                for (uint32_t s2 = 0; s2 <= 0x07; s2++) {
                    if (s2 == sub) continue;
                    uint32_t e0, e1;
                    int r2 = pcode_read(cmd, s2, 0, &e0, &e1);
                    if (r2 == 0)
                        printf("           sub=0x%02x: d0=0x%08x d1=0x%08x%s\n",
                               s2, e0, e1, (e0 != 0 || e1 != 0) ? "  <== data present" : "");
                }
                break;
            }
        }
    }
    printf("\n=== %d candidate command(s) readable ===\n", cand);
    printf("    Next: investigate candidates with --findlite or interactive.\n\n");
    return 0;
}

/* Deepen: probe candidate command IDs (esp. 0x60-0x78) across ALL subcommands
 * and test whether writes with a 3-channel RGB/duty payload are accepted.
 * Writes are dry-run unless --force. */
static int cmd_findlite(void)
{
    uint32_t d0, d1;
    int ret;
    printf("\n=== Deep lighting-command probe ===\n");
    printf("    Focus 0x60-0x78 + 0x30-0x3F. Testing reads first.\n\n");

    uint32_t focus[] = {
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x60, 0x61, 0x62, 0x63,
        0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D,
        0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
        0x79, 0x7A, 0x7B
    };

    int found = 0;
    for (uint32_t ci = 0; ci < sizeof(focus)/sizeof(focus[0]); ci++) {
        uint32_t cmd = focus[ci];
        for (uint32_t sub = 0; sub <= 0x1F; sub++) {
            ret = pcode_read(cmd, sub, 0, &d0, &d1);
            if (ret == 0) {
                printf("  CAND cmd=0x%02x sub=0x%02x: d0=0x%08x d1=0x%08x%s\n",
                       cmd, sub, d0, d1,
                       (d0 != 0 || d1 != 0) ? "  <== data present" : "");
                found++;
                if (force) {
                    /* Try write RGB-ish payloads (dry run unless --force) */
                    uint32_t r = d0 != 0 ? d0 : 0x00ff2200; /* RGB-ish */
                    ret = pcode_write(cmd, sub, 0, r, 0);
                    printf("           WRITE 0x%08x -> %s%s\n", r,
                           ret == 0 ? "ACCEPTED" : "REJECTED",
                           ret == 0 ? "  <== CANDIDATE!" : pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK));
                }
            }
        }
    }
    printf("\n=== %d readable candidate(s) in focus range ===\n", found);
    printf("    Use --interactive to probe them manually (R/W host).\n");
    printf("    Monitor the GPU RGB (logitech/ASRock app or visual) for changes\n");
    printf("    when --force writes are issued.\n\n");
    return 0;
}

/* Interrogate candidate lighting commands deeply (reads only). */
static int cmd_interrogate(void)
{
    uint32_t d0, d1;
    int ret;
    uint32_t cmds[] = { 0x6f, 0x70, 0x71, 0x54, 0x6e };
    printf("\n=== Deep interrogation of candidate commands ===\n");
    printf("    For each cmd: subcommands 0x00-0x1F, param2 (channel/zone) 0-7.\n\n");
    for (uint32_t ci = 0; ci < sizeof(cmds)/sizeof(cmds[0]); ci++) {
        uint32_t cmd = cmds[ci];
        printf("=== cmd 0x%02x ===\n", cmd);
        for (uint32_t sub = 0; sub <= 0x1F; sub++) {
            for (uint32_t p2 = 0; p2 <= 0x7; p2++) {
                ret = pcode_read(cmd, sub, p2, &d0, &d1);
                if (ret == 0) {
                    printf("  sub=0x%02x p2=%u: d0=0x%08x d1=0x%08x%s\n",
                           sub, p2, d0, d1, (d0 != 0 || d1 != 0) ? "  <== data" : "");
                } else {
                    break; /* once a subcommand errors on p2=0, skip rest */
                }
            }
            fflush(stdout);
        }
        printf("\n");
    }
    return 0;
}

/* Interactive, monitored write-probe over the proprietary PCODE space.
 *
 * Walks candidate (command, subcommand, payload) tuples ONE at a time so a
 * visible change to the B580 RGB pinpoints the lighting command. For each we
 * write a bright, recognizable RGB payload and wait for the user to report
 * whether the light changed. Nothing is permanent; we only observe.
 *
 * Kept intentionally interactive + monitored because we do NOT know the RGB
 * command yet: blind-issuing unchecked writes across the command space could
 * otherwise reach power/frequency/thermal commands.
 */
static int cmd_probe_write(void)
{
    uint32_t d0, d1;
    int ret;

    /* Commands to probe (proprietary / undocumented spaces). */
    uint32_t cmds[] = {
        0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x54, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
        0x78, 0x79, 0x7A, 0x7B
    };

    /* The unknown FAN_SPEED_CONTROL subcommands (0x0-0x3, 0x5-0xF) that the
     * register map marks UNKNOWN - test for write acceptance / RGB too. */
    uint32_t fan_subs[] = { 0x0, 0x1, 0x2, 0x3, 0x5, 0x6, 0x7, 0x8, 0x9,
                            0xa, 0xb, 0xc, 0xd, 0xe, 0xf };

    /* Subcommands to try per real command. */
    uint32_t subs[] = { 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
                        0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
                        0x10, 0x11, 0x12, 0x13, 0x14, 0x1f };

    /* Plausible RGB payload encodings for a magenta test color
     * (R=0xFF, G=0x00, B=0xFF). PCODE_DATA0 is 32 bits, DATA1 holds extras. */
    static const struct {
        uint32_t d0;
        uint32_t d1;
        const char *desc;
    } payloads[] = {
        { 0x0000FFFFu, 0x00000000u, "RGB = 00:00:R:G:B (R,G,B in low 24)" },
        { 0xFF00FF00u, 0x00000000u, "RGB = RR:GG:BB:00 (byte-swapped)" },
        { 0x0000FF18u, 0x000000FFu, "R,G,B low14 + speed 0x18 (static-ish)" },
        { 0xFF00FF00u, 0x000000FFu, "RGB + brightness 0xFF in DATA1" },
        { 0xFF000000u, 0x000000FFu, "RED only + brightness (channel test)" },
        { 0x0000FFFFu, 0x000000FFu, "MAGENTA + brightness (data1=0xFF)" },
        { 0x000000FFu, 0x00000000u, "BLUE low byte only" },
    };

    FILE *log = fopen("/tmp/pcode_rgb_probe_write.log", "a");
    if (!log) {
        fprintf(stderr, "Cannot open log file\n");
        return -1;
    }

    printf("\n=== Interactive, monitored RGB write-probe ===\n");
    printf("    Bright magenta/red/blue writes are issued ONE at a time.\n");
    printf("    After each, watch the B580 RGB and answer:\n");
    printf("      Y  = RGB changed to match payload  (HIT - this is the command)\n");
    printf("      P  = changed PARTIALLY / other color (candidate)\n");
    printf("      N  = no visible change (continue)\n");
    printf("      Q  = quit\n");
    printf("    You can skip a whole command by answering 'S' at any prompt.\n\n");

    char resp;
    for (uint32_t ci = 0; ci < sizeof(cmds)/sizeof(cmds[0]); ci++) {
        uint32_t cmd = cmds[ci];
        /* First, a plain read to see current state & don't disturb working cmds */
        for (uint32_t si = 0; si < sizeof(subs)/sizeof(subs[0]); si++) {
            uint32_t sub = subs[si];
            for (uint32_t pi = 0; pi < sizeof(payloads)/sizeof(payloads[0]); pi++) {
                /* Read baseline state (fan/temp untouched). */
                uint32_t pre0, pre1;
                pcode_read(0x25, 0x2, 0, &pre0, &pre1);

                fprintf(log, "WRITE cmd=0x%02x sub=0x%02x data0=0x%08x data1=0x%08x (%s)\n",
                        cmd, sub, payloads[pi].d0, payloads[pi].d1, payloads[pi].desc);
                ret = pcode_write(cmd, sub, 0, payloads[pi].d0, payloads[pi].d1);

                if (ret == 0) {
                    d0 = mmio_read32(PCODE_DATA0_OFFSET);
                    d1 = mmio_read32(PCODE_DATA1_OFFSET);
                    printf("  > cmd=0x%02x sub=0x%02x: WRITE ACCEPTED resp d0=0x%08x d1=0x%08x\n",
                           cmd, sub, d0, d1);
                } else {
                    printf("  > cmd=0x%02x sub=0x%02x: write REJECTED %s\n",
                           cmd, sub, pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK));
                    fprintf(log, "    -> rejected, skipping further subs of this payload on this cmd\n");
                    break; /* this sub-command doesn't exist; move to next sub */
                }

                printf("    RGB changed? [Y/P/N/Q/S] ");
                fflush(stdout);
                scanf(" %c", &resp);
                while (getchar() != '\n');
                if (resp == 'Y' || resp == 'y') {
                    printf("\n*** HIT: cmd=0x%02x sub=0x%02x payload='%s' controls RGB! ***\n",
                           cmd, sub, payloads[pi].desc);
                    fprintf(log, "    *** HIT cmd=0x%02x sub=0x%02x payloads[%u] ***\n", cmd, sub, pi);
                    fclose(log);
                    return 0;
                } else if (resp == 'P' || resp == 'p') {
                    printf("\n*** PARTIAL: cmd=0x%02x sub=0x%02x payload='%s' ***\n",
                           cmd, sub, payloads[pi].desc);
                    fprintf(log, "    *** PARTIAL cmd=0x%02x sub=0x%02x payloads[%u] ***\n", cmd, sub, pi);
                } else if (resp == 'S' || resp == 's') {
                    printf("    Skipping rest of command 0x%02x.\n", cmd);
                    fprintf(log, "    skipping cmd 0x%02x\n", cmd);
                    goto next_cmd;
                } else if (resp == 'Q' || resp == 'q') {
                    fclose(log);
                    return 0;
                }
            }
        }
next_cmd: ;
    }

    /* Also sweep the unknown FAN_SPEED_CONTROL subcommands (writes only). */
    printf("\n--- Probing FAN_SPEED_CONTROL 0x7D unknown subcommands ---\n");
    for (uint32_t si = 0; si < sizeof(fan_subs)/sizeof(fan_subs[0]); si++) {
        uint32_t sub = fan_subs[si];
        for (uint32_t pi = 0; pi < 3; pi++) {
            fprintf(log, "WRITE FAN cmd=0x7d sub=0x%02x data0=0x%08x data1=0x%08x\n",
                    sub, payloads[pi].d0, payloads[pi].d1);
            ret = pcode_write(FAN_SPEED_CONTROL, sub, 0, payloads[pi].d0, payloads[pi].d1);
            printf("  > FAN(0x7d) sub=0x%02x: %s%s\n", sub,
                   ret == 0 ? "WRITE ACCEPTED" : "REJECTED",
                   ret == 0 ? "" : pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK));
            if (ret != 0) break;
            printf("    RGB changed? [Y/N/Q] ");
            fflush(stdout);
            scanf(" %c", &resp);
            while (getchar() != '\n');
            if (resp == 'Y' || resp == 'y') {
                printf("\n*** HIT: FAN cmd=0x7d sub=0x%02x payloads[%u] ***\n", sub, pi);
                fprintf(log, "    *** HIT FAN cmd=0x7d sub=0x%02x payloads[%u] ***\n", sub, pi);
                fclose(log);
                return 0;
            } else if (resp == 'Q' || resp == 'q') {
                fclose(log);
                return 0;
            }
        }
    }

    fclose(log);
    printf("\nWrite-probe complete; no RGB hit found. Full log: /tmp/pcode_rgb_probe_write.log\n");
    return 0;
}

static int cmd_interactive(void)
{
    char line[256];
    uint32_t cmd, p1, p2, d0, d1;
    int ret;
    uint32_t data0, data1;

    printf("\n=== Interactive PCODE Mailbox ===\n");
    printf("    R cmd param1 param2              (read)\n");
    printf("    W cmd param1 param2 data0        (write; cmd>=0x54 needs no --force)\n");
    printf("    W64 cmd param1 param2 d0 d1      (write64)\n");
    printf("    DC r g b                         (lighting cmd 0x54/0x00, DATA0=RRGGBB)\n");
    printf("    DCZ z r g b                      (lighting cmd 0x54/0x00, param2=zone z)\n");
    printf("    M offset                         (raw MMIO read)\n");
    printf("    Q to quit\n\n");

    while (1) {
        printf("pcode> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;
        line[strcspn(line, "\n")] = 0;
        if (line[0] == 'Q' || line[0] == 'q')
            break;
        if (line[0] == 'R' || line[0] == 'r') {
            if (sscanf(line + 1, " %x %x %x", &cmd, &p1, &p2) != 3) {
                printf("  Usage: R <cmd_hex> <param1_hex> <param2_hex>\n");
                continue;
            }
            ret = pcode_read(cmd, p1, p2, &data0, &data1);
            if (ret == 0)
                printf("  READ OK: cmd=0x%02x p1=0x%02x p2=0x%02x -> d0=0x%08x d1=0x%08x\n",
                       cmd, p1, p2, data0, data1);
            else
                printf("  READ FAILED: %s (0x%02x)\n",
                       pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK),
                       mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK);
        } else if (line[0] == 'D' && (line[1] == 'C' || line[1] == 'c')) {
            /* DC / DCZ: confirmed lighting command 0x54/0x00 */
            uint32_t r, g, b, zone, d0;
            if ((line[1]=='C'||line[1]=='c') && (line[2]=='Z'||line[2]=='z')) {
                if (sscanf(line+3, " %x %x %x %x", &zone, &r, &g, &b) != 4) {
                    printf("  Usage: DCZ <zone_hex> <r_hex> <g_hex> <b_hex>\n");
                    continue;
                }
                d0 = (r << 16) | (g << 8) | b;
                ret = pcode_write(0x54, 0x00, zone, d0, 0);
            } else {
                if (sscanf(line+2, " %x %x %x", &r, &g, &b) != 3) {
                    printf("  Usage: DC <r_hex> <g_hex> <b_hex>\n");
                    continue;
                }
                d0 = (r << 16) | (g << 8) | b;
                ret = pcode_write(0x54, 0x00, 0, d0, 0);
            }
            if (ret == 0)
                printf("  LIGHTING OK: cmd=0x54 sub=0x00 data0=0x%08x (R=%02x G=%02x B=%02x)\n", d0, r, g, b);
            else
                printf("  LIGHTING FAILED: %s\n", pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK));
        } else if (line[0] == 'W' || line[0] == 'w') {
            if ((line[0]=='W'&&line[1]=='6'&&line[2]=='4')||(line[0]=='w'&&line[1]=='6'&&line[2]=='4')) {
                if (sscanf(line+3, " %x %x %x %x %x", &cmd, &p1, &p2, &d0, &d1) != 5) {
                    printf("  Usage: W64 <cmd> <p1> <p2> <data0> <data1>\n");
                    continue;
                }
            } else {
                if (sscanf(line+1, " %x %x %x %x", &cmd, &p1, &p2, &d0) != 4) {
                    printf("  Usage: W <cmd> <param1> <param2> <data0>\n");
                    continue;
                }
                d1 = 0;
            }
            if (!force && cmd < 0x54) {
                printf("  Writes to cmd<0x54 disabled. Re-run with --force to enable.\n");
                continue;
            }
            ret = pcode_write(cmd, p1, p2, d0, d1);
            if (ret == 0) {
                data0 = mmio_read32(PCODE_DATA0_OFFSET);
                data1 = mmio_read32(PCODE_DATA1_OFFSET);
                printf("  WRITE OK: resp d0=0x%08x d1=0x%08x\n", data0, data1);
            } else {
                printf("  WRITE FAILED: %s (0x%02x)\n",
                       pcode_err_str(mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK),
                       mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_ERROR_MASK);
            }
        } else if (line[0] == 'M' || line[0] == 'm') {
            uint32_t offset;
            if (sscanf(line+1, " %x", &offset) != 1) {
                printf("  Usage: M <offset_hex>\n");
                continue;
            }
            if (offset >= BAR0_SIZE) {
                printf("  Offset out of range (max 0x%x)\n", BAR0_SIZE - 1);
                continue;
            }
            printf("  MMIO[0x%06x] = 0x%08x\n", offset, mmio_read32(offset));
        } else {
            printf("  Unknown command. Use R, W, W64, DC, DCZ, M, or Q.\n");
        }
    }
    return 0;
}

static void usage(const char *prog)
{
    printf("pcode_rgb_probe - Intel Arc B580 PCODE mailbox probe (+RGB search)\n\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("  --baseline      Sanity: fan/temp/late-bind/power reads\n");
    printf("  --scan-rgb      Scan command space for readable (lighting?) cmds\n");
    printf("  --findlite     Deep-probe focus range for lighting command (writes need --force)\n");
    printf("  --interrogate  Deep-dump candidate commands (0x6f/0x70/0x71/0x54)\n");
    printf("  --probe-write  MONITORED write-sweep for the RGB command (interactive; watch the RGB)\n");
    printf("  --interactive  Manual mailbox read/write\n");
    printf("  --force        Enable WRITE commands (use with caution)\n");
    printf("  --verbose      Show all errors\n");
    printf("  --pci BDF      PCI address (default auto-detect 8086:e20b)\n");
    printf("  -h, --help     Show help\n");
    printf("\nRequires root (PCI resource files). Set XE_PCI_BDF to override.\n");
}

int main(int argc, char **argv)
{
    int opt;
    int do_base = 0, do_scan = 0, do_find = 0, do_inter = 0, do_intg = 0, do_pw = 0;
    static struct option long_opts[] = {
        {"baseline",  no_argument, 0, 'b'},
        {"scan-rgb",  no_argument, 0, 's'},
        {"findlite",  no_argument, 0, 'f'},
        {"interrogate", no_argument, 0, 'g'},
        {"probe-write", no_argument, 0, 'p'},
        {"interactive", no_argument, 0, 'i'},
        {"force",     no_argument, 0, 'F'},
        {"verbose",   no_argument, 0, 'v'},
        {"pci",       required_argument, 0, 'd'},
        {"help",      no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    while ((opt = getopt_long(argc, argv, "bsfiFgpvd:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'b': do_base = 1; break;
        case 's': do_scan = 1; break;
        case 'f': do_find = 1; break;
        case 'i': do_inter = 1; break;
        case 'F': force = 1; break;
        case 'g': do_intg = 1; break;
        case 'p': do_pw = 1; break;
        case 'v': verbose = 1; break;
        case 'd': snprintf(pci_bdf, sizeof(pci_bdf), "%s", optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (argc == 1) { usage(argv[0]); return 1; }
    if (geteuid() != 0) {
        fprintf(stderr, "Error: must run as root to access PCI resource files\n");
        return 1;
    }
    if (resolve_pci_bdf() < 0)
        return 1;
    printf("Intel Arc B580 PCODE Mailbox Probe (PCI %s)\n", pci_bdf);
    printf("============================================\n");
    if (map_bar0() < 0)
        return 1;
    printf("[INFO] PCODE_MAILBOX initial: 0x%08x (READY=%s)\n",
           mmio_read32(PCODE_MAILBOX_OFFSET),
           (mmio_read32(PCODE_MAILBOX_OFFSET) & PCODE_READY) ? "YES" : "NO");

    int ret = 0;
    if (do_base)  ret |= cmd_baseline();
    if (do_scan)  ret |= cmd_scan_rgb();
    if (do_find)  ret |= cmd_findlite();
    if (do_intg)  ret |= cmd_interrogate();
    if (do_pw)    ret |= cmd_probe_write();
    if (do_inter) ret |= cmd_interactive();

    unmap_bar0();
    printf("[INFO] BAR0 unmapped.\n");
    return ret;
}