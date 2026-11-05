// SPDX-License-Identifier: GPL-2.0+
#include <dm.h>
#include <dm/platdata.h>
#include <mmc.h>
#include <sdhci.h>
#include <command.h>
#include <dm/lists.h>
#include <dm/device-internal.h>
#include <asm/global_data.h>
#include <time.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <asm/io.h>
#include "power.h"
DECLARE_GLOBAL_DATA_PTR;

struct totoro_mmc_plat { struct mmc_config cfg; struct mmc mmc; };
static struct dm_mmc_ops totoro_mmc_ops;
static int totoro_mmc_get_cd(struct udevice *dev)
{
	/* Socket switch is GPIO16, active low, separate from SDHCI state. */
	return !(readl((void *)0x088ce018) & (1 << 16));
}
static int totoro_mmc_bind(struct udevice *dev)
{
	struct totoro_mmc_plat *p = dev_get_plat(dev);
	totoro_mmc_ops = sdhci_ops;
	totoro_mmc_ops.get_cd = totoro_mmc_get_cd;
	return sdhci_bind(dev, &p->mmc, &p->cfg);
}
static int totoro_mmc_probe(struct udevice *dev)
{
	struct totoro_mmc_plat *p = dev_get_plat(dev);
	struct sdhci_host *h = dev_get_priv(dev);
	struct mmc_uclass_priv *u = dev_get_uclass_priv(dev);
	int ret;
	unsigned int spins;
	unsigned long long before = get_ticks();
	for (spins = 0; spins < 100000; spins++)
		if (get_ticks() != before) break;
	if (spins == 100000) {
		puts("SD skipped: GPT counter stopped\n");
		return -ETIMEDOUT;
	}
	clrbits_le32((void *)0x088ce004, 3); /* GPIO16 input */
	setbits_le32((void *)0x088ce028, 1 << 16); /* pull up */
	setbits_le32((void *)0x088ce020, 1 << 16);
	ret = totoro_sd_power_on();
	if (ret) return ret;
	/* SDHC3 pinmux, pull-ups and 6mA drive, as on the vendor Totoro board. */
	clrbits_le32((void *)0x08880000, 3);
	clrsetbits_le32((void *)0x0888000c, (3 << 19) | (3 << 22),
			 (1 << 19) | (1 << 22));
	clrsetbits_le32((void *)0x08880020, (7 << 6) | (7 << 9),
			 (4 << 6) | (4 << 9));
	writel(1, (void *)0x08880154); /* SDIO3 AHB clock */
	writel(0, (void *)0x081401d0); /* 104MHz / 2 = 52MHz */
	setbits_le32((void *)0x081401cc, 1);
	writel(2, (void *)0x08408000); /* Broadcom wrapper core reset */
	mdelay(5);
	writel(0, (void *)0x08408000);
	writel(1, (void *)0x08408004); /* Feed card-detect into the SDHCI core. */
	writel(1, (void *)0x08408008);
	h->name = "BCM21553 SDHC3";
	h->ioaddr = (void *)0x08400000;
	h->bus_width = 4;
	h->max_clk = 52000000;
	h->mmc = &p->mmc;
	h->mmc->dev = dev;
	h->mmc->priv = h;
	ret = sdhci_setup_cfg(&p->cfg, h, 25000000, 400000);
	if (ret) {
		printf("SD caps=%08x state=%08x\n", sdhci_readl(h, SDHCI_CAPABILITIES),
		       sdhci_readl(h, SDHCI_PRESENT_STATE));
		return ret;
	}
	u->mmc = h->mmc;
	return sdhci_probe(dev);
}
U_BOOT_DRIVER(totoro_mmc) = {
	.name = "totoro_mmc", .id = UCLASS_MMC,
	.bind = totoro_mmc_bind, .probe = totoro_mmc_probe,
	.ops = &totoro_mmc_ops, .priv_auto = sizeof(struct sdhci_host),
	.plat_auto = sizeof(struct totoro_mmc_plat),
};
/* Bind only on explicit request: a gated SD host must not block the menu. */
static int do_sdinit(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	static struct udevice *dev;
	struct mmc *mmc;
	int ret;
	if (!dev) {
		puts("Starting SDHC3...\n");
		ret = device_bind_driver(gd->dm_root, "totoro_mmc", "sd0", &dev);
		if (ret) {
			dev = NULL;
			printf("SD bind failed: %d\n", ret);
			return CMD_RET_FAILURE;
		}
	}
	/* find_mmc_device() does not probe a controller bound after startup. */
	ret = device_probe(dev);
	if (ret) {
		printf("SD probe failed: %d\n", ret);
		return CMD_RET_FAILURE;
	}
	mmc = mmc_get_mmc_dev(dev);
	printf("SD state=%08x CD=%d\n", readl((void *)0x08400024), mmc_getcd(mmc));
	ret = mmc_init(mmc);
	if (ret) {
		printf("SD card init error: %d\n", ret);
		printf("IRQ=%08x PWR=%02x\n", readl((void *)0x08400030),
		       readb((void *)0x08400029));
	} else {
		printf("SD ready: %llu MiB\n", (unsigned long long)(mmc->capacity >> 20));
	}
	return ret ? CMD_RET_FAILURE : 0;
}
U_BOOT_CMD(sdinit, 1, 0, do_sdinit, "initialize optional SD controller", "");
