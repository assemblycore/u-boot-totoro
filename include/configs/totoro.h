/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __CONFIG_TOTORO_H
#define __CONFIG_TOTORO_H
#define CFG_SYS_SDRAM_BASE 0x80000000
#define CFG_EXTRA_ENV_SETTINGS "stdin=serial\0stdout=serial\0stderr=serial\0" \
	"fastboot_bootcmd=go 0x82000000\0"
#endif
