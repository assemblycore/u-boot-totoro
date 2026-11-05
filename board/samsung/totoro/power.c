// SPDX-License-Identifier: GPL-2.0+
/* Polled BCM21553 BSC1 and MAX8986 DLDO1 (microSD supply).
 * Register definitions/sequence: vendor bcm_i2c.h, i2c-bcm1161.c,
 * max8986.h, max8986-regulator.c and board-bcm21553.c.
 */
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <stdio.h>
#include "power.h"

#define BSC ((u8 *)0x088a0020)
#define CS 0x00
#define DATA 0x08
#define ISR 0x28
#define PMIC_ADDR 0x08
#define D1_MODE 0x17
#define A1_D1_VOLTAGE 0xb3

static int bsc_wait(void)
{
	unsigned int n;
	u8 status;
	for (n = 0; n < 1000; n++) {
		status = readb(BSC + ISR);
		if (status & 4) return -EIO;
		if (status & 8) return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int bsc_command(u8 cmd)
{
	int ret;
	writeb(0x0f, BSC + ISR);
	writeb(0xc1 | cmd, BSC + CS);
	ret = bsc_wait();
	writeb(0xc1, BSC + CS); /* clear the completed command */
	return ret;
}

static int bsc_send(u8 byte)
{
	int ret;
	writeb(0x0f, BSC + ISR);
	writeb(0xc1, BSC + CS);
	writeb(byte, BSC + DATA);
	ret = bsc_wait();
	if (!ret && (readb(BSC + CS) & 8)) ret = -EREMOTEIO;
	return ret;
}

static int pmic_reg(u8 reg, u8 *value, bool write)
{
	int ret, stop;
	ret = bsc_command(2); /* START */
	if (!ret) ret = bsc_send(PMIC_ADDR << 1);
	if (!ret) ret = bsc_send(reg);
	if (write) {
		if (!ret) ret = bsc_send(*value);
	} else {
		if (!ret) ret = bsc_command(0x0a); /* repeated START */
		if (!ret) ret = bsc_send((PMIC_ADDR << 1) | 1);
		if (!ret) ret = bsc_command(0x0e); /* read one byte, NACK */
		if (!ret) *value = readb(BSC + DATA);
	}
	stop = bsc_command(4);
	return ret ? ret : stop;
}

int totoro_sd_power_on(void)
{
	u8 id, voltage, mode, check, old_mode;
	int ret;
	/* Vendor 13MHz BSC input and 100kHz bus, polling without IRQs. */
	writel(0, (void *)0x081401a0);
	writel(1, (void *)0x081401b8);
	writeb(0xc0, BSC + CS);
	writeb(0, BSC + 0x24);
	writeb(0, BSC + 0x10); /* CRC disabled */
	writeb(0xf2, BSC + 0x04);
	writeb(0x93, BSC + 0x2c); /* clock, M=1 N=1, autosense */
	writeb(0xff, BSC + 0x0c);
	writeb(0x80, BSC + 0x1c); /* flush TX FIFO */
	writeb(0x0f, BSC + ISR);
	writeb(0xc1, BSC + CS);
	udelay(100);
	ret = pmic_reg(0, &id, false);
	if (ret) goto fail;
	/* The hardware-tested Totoro PMIC reports 0x32. Do not write an
	 * unidentified chip at this address. */
	if (id != 0x32) {
		printf("Unexpected PMIC ID: %02x\n", id);
		return -ENODEV;
	}
	ret = pmic_reg(D1_MODE, &old_mode, false);
	if (ret) goto fail;
	ret = pmic_reg(A1_D1_VOLTAGE, &voltage, false);
	if (ret) goto fail;
	/* DLDO1 selector 7 = 3.0V. Preserve ALDO1 in the shared register. */
	voltage = (voltage & ~0x70) | 0x70;
	ret = pmic_reg(A1_D1_VOLTAGE, &voltage, true);
	if (ret) goto fail;
	mode = 0; /* ON in all PC1/PC2 states, vendor Totoro default. */
	ret = pmic_reg(D1_MODE, &mode, true);
	if (ret) goto fail;
	ret = pmic_reg(D1_MODE, &check, false);
	if (!ret && check != mode) ret = -EIO;
	if (ret) goto fail;
	ret = pmic_reg(A1_D1_VOLTAGE, &check, false);
	if (!ret && check != voltage) ret = -EIO;
	if (ret) goto fail;
	mdelay(20);
	printf("SD power 3.0V: %02x -> 00\n", old_mode);
	return 0;
fail:
	printf("SD PMIC error: %d\n", ret);
	return ret;
}
