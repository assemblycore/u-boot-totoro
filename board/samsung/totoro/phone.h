/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef TOTORO_PHONE_H
#define TOTORO_PHONE_H
#include <linux/types.h>
int totoro_menu(const char *title, const char *const *items, unsigned int count);
int totoro_phone_menu(bool boot);
struct blk_desc;
int totoro_script(struct blk_desc *desc, int part, const char *path);
int totoro_script_menu(struct blk_desc *desc, int part);
int totoro_sd_menu(void);
void totoro_usb_release_console(void);
#endif
