// SPDX-License-Identifier: GPL-2.0+
/* BCM21553 HSOTG address from the vendor mach-bcm215xx hardware.h.
 * Clock/reset/ANACR9 sequence follows the vendor kernel's USB bring-up.
 */
#include <usb.h>
#include <usb/dwc2_udc.h>
#include <asm/io.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <command.h>
static struct dwc2_plat_otg_data pdata = {
	.regs_otg = 0x08200000,
	.rx_fifo_sz = 256, .np_tx_fifo_sz = 128, .tx_fifo_sz = 128,
	/* VBUS detection belongs to MAX8986 on this peripheral-only board. */
	.force_b_session_valid = true,
};
static bool ready;
int board_usb_init(int index, enum usb_init_type init)
{
	u32 id;
	if (index || init != USB_INIT_DEVICE) return -ENODEV;
	if (ready) return 0;
	writel(1, (void *)0x08880108); /* USB AHB clock */
	writel(1, (void *)0x081400a4); /* 48MHz PLL power */
	writel(1, (void *)0x081400a8); /* PLL output gate */
	writel(1, (void *)0x081401d4); /* USB 48MHz gate */
	setbits_le32((void *)0x08280000, 0x18000);
	clrbits_le32((void *)0x08280000, 0xc00);
	setbits_le32((void *)0x08280000, 0x8c00);
	clrbits_le32((void *)0x08140140, 2);
	writel(0x17f, (void *)0x088800a4);
	writel(0x15f, (void *)0x088800a4);
	writel(0x17f, (void *)0x088800a4);
	udelay(1000);
	id = readl((void *)0x08200040);
	if ((id >> 16) != 0x4f54) {
		printf("HSOTG core unavailable: ID=%08x\n", id);
		return -ENODEV;
	}
	/* Explicit 8-bit UTMI HS interface; do not inherit SBL's PHY width
	 * or turnaround time. Vendor PCD uses 9 clocks for cores >= 2.60a. */
	pdata.usb_gusbcfg = (readl((void *)0x0820000c) &
		~((1U << 29) | (0xf << 10) | (1 << 15) | (1 << 7) |
		  (1 << 6) | (1 << 4) | (1 << 3) | 7)) |
		(1U << 30) | ((id >= 0x4f54260a ? 9 : 8) << 10) | 7;
	/* Establish device role/PHY width before the generic driver resets it. */
	writel(pdata.usb_gusbcfg, (void *)0x0820000c);
	mdelay(25);
	printf("USB core=%08x cfg=%08x\n", id, pdata.usb_gusbcfg);
	int ret = dwc2_udc_probe(&pdata);
	ready = !ret;
	return ret;
}
int board_usb_cleanup(int index, enum usb_init_type init) { return 0; }

static int do_usbdiag(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	dwc2_udc_dump_status();
	return 0;
}
U_BOOT_CMD(usbdiag, 1, 0, do_usbdiag, "show USB device state", "");
