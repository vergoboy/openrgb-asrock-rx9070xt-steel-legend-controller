/*
 * amc_i2c.c - Linux I2C client driver for the ASRock Arc B580 Steel Legend
 *             GPU Add-on Management Controller (AMC), Nuvoton NUC121, at
 *             I2C address 0x36 on the discrete GPU's DesignWare bus.
 *
 * The xe driver registers this client with the name "amc" but does not bind
 * any driver, so the controller is not reachable from userspace on Linux.
 * This small client binds that node and exposes a sysfs interface so the AMC
 * registers can be inspected/commanded - the Linux analogue of ASRock's
 * AsrNfDrv000.sys on Windows.
 *
 * Sysfs interface (under /sys/bus/i2c/devices/<bus>-0036/):
 *   addr          : (ro) the client I2C address, e.g. 0x36
 *   dump          : (ro) hex of the last stored block (all zeros by default)
 *   reg           : (rw) 8-bit register window base (0-0xFF)
 *   value         : (rw) byte read/written at the current register window
 *   wr            : (wo) hex bytes to send as a raw block write (e.g. the
 *                        12-byte 0x10 color packet)
 *   rd           : (ro) raw 1..63 byte read (hex), empty until wr/read used
 *
 * All access redirects to the driver's i2c_xfer on the client's adapter so
 * reads only run while the adapter is usable under xe.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/kstrtox.h>
#include <linux/uaccess.h>

#define DRV_NAME "amc_i2c"

struct amc_i2c_data {
	u16 reg;                /* current register-window base (16-bit)  */
	u8 value;               /* last value read at the window      */
	u8 block[63];           /* last raw block written/read        */
	u8 block_len;
	struct i2c_client *client;
};

/* ---------------- helpers ---------------- */

static int amc_i2c_read_byte(struct i2c_client *client, u16 reg, u8 *out)
{
	struct i2c_msg msgs[2];
	u8 wbuf[2] = { (reg >> 8) & 0xff, reg & 0xff };

	msgs[0].addr  = client->addr;
	msgs[0].flags = 0;
	msgs[0].len   = 2;
	msgs[0].buf   = wbuf;

	msgs[1].addr  = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len   = 1;
	msgs[1].buf   = out;

	return i2c_transfer(client->adapter, msgs, 2) == 2 ? 0 : -EIO;
}

static int amc_i2c_write_byte(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3] = { (reg >> 8) & 0xff, reg & 0xff, val };
	return i2c_master_send(client, buf, 3) < 0 ? -EIO : 0;
}

/* ---------------- sysfs ---------------- */

static ssize_t addr_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct amc_i2c_data *d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "0x%02x\n", d->client->addr);
}

static ssize_t reg_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct amc_i2c_data *d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "0x%02x\n", d->reg);
}

static ssize_t reg_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct amc_i2c_data *d = dev_get_drvdata(dev);
	u32 v;

	if (kstrtou32(buf, 0, &v) || v > 0xFFFF)
		return -EINVAL;
	d->reg = (u16)v;
	return count;
}

static ssize_t value_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct amc_i2c_data *d = dev_get_drvdata(dev);
	int ret = amc_i2c_read_byte(d->client, d->reg, &d->value);

	if (ret) {
		return sysfs_emit(buf, "read-error %d\n", ret);
	}
	return sysfs_emit(buf, "0x%02x\n", d->value);
}

static ssize_t value_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct amc_i2c_data *d = dev_get_drvdata(dev);
	u32 v;
	int ret;

	if (kstrtou32(buf, 0, &v) || v > 0xFF)
		return -EINVAL;
	ret = amc_i2c_write_byte(d->client, d->reg, (u8)v);
	if (ret)
		return ret;
	d->value = (u8)v;
	return count;
}

static ssize_t rd_show(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct amc_i2c_data *d = dev_get_drvdata(dev);
	int i;
	char *p = buf;

	for (i = 0; i < d->block_len; i++)
		p += sprintf(p, "%02x", d->block[i]);
	p += sprintf(p, "\n");
	return p - buf;
}

static ssize_t wr_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct amc_i2c_data *d = dev_get_drvdata(dev);
	int i, n = 0;
	u8 tmp[63];

	/* parse hex bytes */
	while (n < 63 && (buf[n] && (buf[n] == ' ' || buf[n] == '\t')))
		n++;
	for (i = 0; i < 63 && n < count; ) {
		char hb[3] = {0};
		unsigned long v;
		int base = n;

		while (buf[n] == ' ' || buf[n] == '\t')
			n++;
		if (buf[n] == '\n' || buf[n] == '\0')
			break;

		hb[0] = buf[n];
		hb[1] = buf[n + 1];
		if (buf[n + 1] == ' ' || buf[n + 1] == '\t' || buf[n + 1] == '\n' ||
		    buf[n + 1] == '\0') {
			/* single hex digit */
			if (kstrtoul(hb, 16, &v) || v > 0xFF)
				return -EINVAL;
			tmp[i++] = (u8)v;
			n = base + 1;
			continue;
		}
		if (kstrtoul(hb, 16, &v) || v > 0xFF)
			return -EINVAL;
		tmp[i++] = (u8)v;
		n = base + 2;
	}
	if (i == 0)
		return -EINVAL;

	memcpy(d->block, tmp, i);
	d->block_len = i;

	/* optional single read-after-write is not implicit; just send block */
	if (i2c_master_send(d->client, d->block, i) < 0)
		return -EIO;

	return count;
}

static ssize_t dump_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct amc_i2c_data *d = dev_get_drvdata(dev);
	int i;
	char *p = buf;

	for (i = 0; i < d->block_len; i++)
		p += sprintf(p, "%02x%c", d->block[i],
			     (i == d->block_len - 1) ? '\n' : ' ');
	return p - buf;
}

static DEVICE_ATTR_RO(addr);
static DEVICE_ATTR_RW(reg);
static DEVICE_ATTR_RW(value);
static DEVICE_ATTR_RO(rd);
static DEVICE_ATTR_WO(wr);
static DEVICE_ATTR_RO(dump);

static struct attribute *amc_i2c_attrs[] = {
	&dev_attr_addr.attr,
	&dev_attr_reg.attr,
	&dev_attr_value.attr,
	&dev_attr_rd.attr,
	&dev_attr_wr.attr,
	&dev_attr_dump.attr,
	NULL
};
ATTRIBUTE_GROUPS(amc_i2c);

/* ---------------- driver ---------------- */

static int amc_i2c_probe(struct i2c_client *client)
{
	struct amc_i2c_data *d;
	int ret;

	dev_info(&client->dev, "probing AMC at 0x%02x\n", client->addr);

	d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->client = client;
	dev_set_drvdata(&client->dev, d);

	ret = device_add_groups(&client->dev, amc_i2c_groups);
	if (ret)
		dev_err(&client->dev, "failed to add sysfs groups: %d\n", ret);

	return ret;
}

static const struct i2c_device_id amc_i2c_id[] = {
	{ "amc", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, amc_i2c_id);

static struct i2c_driver amc_i2c_driver = {
	.driver = {
		.name           = DRV_NAME,
	},
	.probe       = amc_i2c_probe,
	.id_table    = amc_i2c_id,
};

module_i2c_driver(amc_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ASRock Arc B580 Linux rg");
MODULE_DESCRIPTION("I2C client driver exposing the Arc B580 GPU AMC to sysfs");
MODULE_VERSION("0.1");