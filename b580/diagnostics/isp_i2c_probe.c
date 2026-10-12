/*
 * isp_i2c_probe.c
 *
 * Read-only ISP ("In-System Programming") probe for the Nuvoton NUC121
 * RGB MCU on the B580's SPB I2C bus (/dev/i2c-28, slave address 0x36).
 *
 * Implements the OpenNuvoton ISPLib ISP-over-I2C framing (verified against
 * ASRock's asrspb.dll / AsrISP.exe transport) but ONLY issues read/query
 * commands. It does NOT erase or write any flash:
 *    - Connect       0xAE  (handshake / returns device ID + ack)
 *    - GetDeviceID   0xB1  (should return 0x00012145 for NUC121ZC2AE)
 *    - GetVersion    0xA6  (ISP firmware version, 1 byte)
 *
 * Wire frame (64 bytes written to the slave):
 *     [0..3]  uCmd      (LE u32 command constant)
 *     [4..7]  cmdIndex  (LE u32 sequence counter)
 *     [8..63] payload   (zero padded; none for these query commands)
 * Response is read back in a second I2C message on the same transaction,
 * mirroring asrspb.dll's AsrSpb_WriteRead.
 *
 * Build:  make isp_i2c_probe
 * Run:    sudo ./isp_i2c_probe [bus] [addr]
 *         (defaults: bus 28, addr 0x36)
 *
 * This is intentionally query-only. Nothing is ever erased or programmed.
 */

#define _GNU_SOURCE
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

#define FRAME_LEN  64
#define RESP_LEN   64

#define CMD_CONNECT       0x000000AEu
#define CMD_GET_VERSION   0x000000A6u
#define CMD_GET_DEVICEID  0x000000B1u

/* Expected NUC121ZC2AE device ID (OpenNuvoton PartNumID.py) */
#define NUC121ZC2AE_DID  0x00012145u

/* ISPLib io_handle_t state */
static unsigned int   s_index;        /* m_uCmdIndex */
static unsigned short s_checksum;     /* m_usCheckSum */
static unsigned int   s_resend;       /* bResendFlag */

static unsigned short checksum(const unsigned char *buf, unsigned int len)
{
    unsigned int i;
    unsigned short c;
    for (c = 0, i = 0; i < len; i++)
        c += buf[i];
    return c;
}

static unsigned int s_framelen = FRAME_LEN;   /* bytes actually written     */
static unsigned int s_compact   = 0;           /* 1 = only 8+payload, no pad  */
static unsigned int s_force     = 0;           /* 1 = use I2C_SLAVE_FORCE      */
static unsigned int s_raw1      = 0;           /* 1 = single-byte read probe    */

/* Build the on-wire frame for uCmd (+ optional payload). */
static void build_frame(unsigned int uCmd,
                        const unsigned char *payload, unsigned int paylen,
                        unsigned char out[FRAME_LEN])
{
    unsigned int len;
    memset(out, 0, FRAME_LEN);
    /* ac_buffer[1..64] are what actually goes on the I2C wire (isplib_i2c). */
    memcpy(&out[0], &uCmd, 4);
    memcpy(&out[4], &s_index, 4);
    if (payload && paylen) {
        if (paylen > FRAME_LEN - 8)
            paylen = FRAME_LEN - 8;
        memcpy(&out[8], payload, paylen);
    }
    if (s_compact)
        len = 8 + paylen;
    else
        len = FRAME_LEN;
    if (len > s_framelen)
        len = s_framelen;
    s_checksum = checksum(out, len);
}

/* One-shot write to the slave (ISPLib m_dev_io.write). */
static int i2c_write(int fd, uint8_t addr, const unsigned char *tx, unsigned int tx_len)
{
    struct i2c_msg msgs[1];
    struct i2c_rdwr_ioctl_data rdwr;

    msgs[0].addr  = addr;
    msgs[0].flags = 0;                 /* I2C_M_WRITE */
    msgs[0].len   = (uint16_t)tx_len;
    msgs[0].buf   = (uint8_t *)tx;

    rdwr.msgs  = msgs;
    rdwr.nmsgs = 1;

    return ioctl(fd, I2C_RDWR, &rdwr);
}

/* One-shot read from the slave (ISPLib m_dev_io.read). */
static int i2c_read(int fd, uint8_t addr, unsigned char *rx, unsigned int rx_len)
{
    struct i2c_msg msgs[1];
    struct i2c_rdwr_ioctl_data rdwr;

    msgs[0].addr  = addr;
    msgs[0].flags = I2C_M_RD;
    msgs[0].len   = (uint16_t)rx_len;
    msgs[0].buf   = rx;

    rdwr.msgs  = msgs;
    rdwr.nmsgs = 1;

    return ioctl(fd, I2C_RDWR, &rdwr);
}

/* ISPLib ISP_Write: sends one command frame. */
static int isp_write(int fd, uint8_t addr, unsigned int uCmd,
                     const unsigned char *payload, unsigned int paylen)
{
    unsigned char frame[FRAME_LEN];
    int ret;

    build_frame(uCmd, payload, paylen, frame);
    ret = i2c_write(fd, addr, frame, s_framelen);
    if (ret < 0) {
        printf("  [write 0x%08X] errno=%d %s (frame %u bytes)\n",
               uCmd, errno, strerror(errno), s_framelen);
        return 0;
    }
    s_index += 2;                      /* matches ISPLib m_uCmdIndex += 2 */
    return 1;
}

/* ISPLib ISP_Read: reads the response to the last write. */
static int isp_read(int fd, uint8_t addr, unsigned char *out, unsigned int maxlen,
                    unsigned int bCheckIndex)
{
    unsigned char buf[RESP_LEN];
    unsigned short usCheckSum;
    unsigned int uCmdIndex, dwLength, szMaxLen;
    int ret, i;

    memset(buf, 0, sizeof(buf));
    ret = i2c_read(fd, addr, buf, RESP_LEN);
    if (ret < 0) {
        printf("  [read] errno=%d %s\n", errno, strerror(errno));
        return 0;
    }

    printf("  [read] raw %u bytes:", RESP_LEN);
    for (i = 0; i < RESP_LEN; i++)
        printf(" %02X", buf[i]);
    printf("\n");

    /* The slave replies with [1..2]=checksum, [5..8]=index echo (ISPLib
       ISP_Read parses at those offsets because byte[0] is the 0 pad). */
    usCheckSum = *((unsigned short *)&buf[1]);
    uCmdIndex  = *((unsigned int *)&buf[5]);

    dwLength = RESP_LEN;
    if (dwLength < 8)
        return 0;
    if (bCheckIndex && uCmdIndex != s_index - 1)
    { s_resend = 1; return 0; }
    if (usCheckSum != s_checksum)
    { s_resend = 1; return 0; }

    szMaxLen = (maxlen > dwLength - 8) ? (dwLength - 8) : maxlen;
    if (out && szMaxLen > 0)
        memcpy(out, &buf[9], szMaxLen);
    s_resend = 0;
    return 1;
}

/* ISP_Connect – hands the MCU the 0xAE command and reads its ID. */
static int isp_connect(int fd, uint8_t addr)
{
    unsigned int uID = 0;
    int ret = 0;

    if (isp_write(fd, addr, CMD_CONNECT, NULL, 0))
        ret = isp_read(fd, addr, (unsigned char *)&uID, 4, 0);
    if (ret)
        s_index = 3;
    return ret ? uID : 0;
}



int main(int argc, char **argv)
{
int    bus  = 28;
    uint8_t addr = 0x36;
    char   devpath[64];
    int    fd;
    unsigned int did;
    unsigned char ver = 0;

    if (argc >= 2) bus  = atoi(argv[1]);
    if (argc >= 3) addr = (uint8_t)strtoul(argv[2], NULL, 0);
    if (argc >= 4) s_framelen = (unsigned int)strtoul(argv[3], NULL, 0);
    if (argc >= 5) s_compact   = (unsigned int)strtoul(argv[4], NULL, 0);
    if (argc >= 6) s_force     = (unsigned int)strtoul(argv[5], NULL, 0);
    if (argc >= 7) s_raw1      = (unsigned int)strtoul(argv[6], NULL, 0);

    snprintf(devpath, sizeof(devpath), "/dev/i2c-%d", bus);
    fd = open(devpath, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", devpath, strerror(errno));
        return 1;
    }
    printf("ISP read-only probe: %s addr 0x%02X (query-only, no flash write)\n",
           devpath, addr);
    printf("  write frame: len=%u compact=%u\n", s_framelen, s_compact);

    if (s_force) {
        /* I2C_SLAVE_FORCE bypasses the kernel's client-registration gate so we
           can see if the bus physically ACKs 0x36 at all. Reversible per-fd. */
        if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0) {
            fprintf(stderr, "I2C_SLAVE_FORCE 0x%02X: %s\n", addr, strerror(errno));
            close(fd);
            return 1;
        }
        printf("  I2C_SLAVE_FORCE 0x%02X set\n", addr);
    }

    if (s_raw1) {
        /* Plain single-byte read at addr: does the bus ACK any read here? */
        unsigned char b = 0;
        int r = i2c_read(fd, addr, &b, 1);
        printf("  [raw1 read 1 byte] ret=%d errno=%d %s byte=0x%02X%s\n",
               r, errno, strerror(errno), b,
               r < 0 ? "" : (b == 0xFF ? " (all-ones, likely NACK pad)" : " (real data?)"));
        close(fd);
        return 0;
    }

    /* Connect */
    s_index = 0;
    did = isp_connect(fd, addr);
    if (!did) {
        printf("ISP Connect (0xAE) FAILED - no response. Raw framing may "
               "differ, MCU may not be in ISP/APROM mode, or bus is blocked.\n");
        printf("(A healthy NUC121 in ISP mode returns DID 0x%08X on connect.)\n",
               NUC121ZC2AE_DID);
        goto out;
    }
    printf("ISP Connect OK - returned device ID 0x%08X%s\n", did,
           (did == NUC121ZC2AE_DID) ? " (NUC121ZC2AE match!)" : " (unexpected)");

    /* GetDeviceID (0xB1) */
    s_index = 3;
    did = 0;
    if (isp_write(fd, addr, CMD_GET_DEVICEID, NULL, 0) &&
        isp_read(fd, addr, (unsigned char *)&did, 4, 1))
        printf("GetDeviceID (0xB1) = 0x%08X%s\n", did,
               (did == NUC121ZC2AE_DID) ? " (NUC121ZC2AE match!)" : "");
    else
        printf("GetDeviceID (0xB1) read failed.\n");

    /* GetVersion (0xA6) */
    s_index = 5;
    if (isp_write(fd, addr, CMD_GET_VERSION, NULL, 0) &&
        isp_read(fd, addr, &ver, 1, 1))
        printf("GetVersion (0xA6)  = 0x%02X\n", ver);
    else
        printf("GetVersion (0xA6) read failed.\n");

out:
    close(fd);
    return 0;
}