// SPDX-License-Identifier: GPL-2.0+
/* Filesystem script boot shared by the SD and resident-STL block devices. */
#include <blk.h>
#include <command.h>
#include <env.h>
#include <fs.h>
#include <image.h>
#include <mmc.h>
#include <part.h>
#include <asm/unaligned.h>
#include <linux/errno.h>
#include "phone.h"

#define SCRIPT_ADDR 0x83000000UL
#define SCRIPT_MAX 0x100000

int totoro_script(struct blk_desc *desc, int part, const char *path)
{
	struct legacy_img_hdr *hdr = (void *)SCRIPT_ADDR;
	loff_t size, actual;
	char devpart[24];
	int ret;
	if (fs_set_blk_dev_with_part(desc, part) || fs_size(path, &size)) {
		printf("Script not found: %s\n", path);
		return -ENOENT;
	}
	if (size < sizeof(*hdr) + 8 || size > SCRIPT_MAX) return -EFBIG;
	if (fs_set_blk_dev_with_part(desc, part)) return -ENODEV;
	ret = fs_read(path, SCRIPT_ADDR, 0, size, &actual);
	if (ret || actual != size) return -EIO;
	/* Validate bounds before source walks the legacy script image. */
	if (!image_check_magic(hdr) || !image_check_hcrc(hdr) ||
	    !image_check_type(hdr, IH_TYPE_SCRIPT) ||
	    image_get_comp(hdr) != IH_COMP_NONE ||
	    image_get_data_size(hdr) != size - sizeof(*hdr) ||
	    !image_check_dcrc(hdr) ||
	    !get_unaligned_be32((u8 *)hdr + sizeof(*hdr)) ||
	    get_unaligned_be32((u8 *)hdr + sizeof(*hdr)) > size - sizeof(*hdr) - 8 ||
	    get_unaligned_be32((u8 *)hdr + sizeof(*hdr) + 4)) {
		puts("Invalid boot.scr (use mkimage -T script)\n");
		return -ENOEXEC;
	}
	snprintf(devpart, sizeof(devpart), "%x:%x", desc->devnum, part);
	env_set("bootdev", blk_get_uclass_name(desc->uclass_id));
	env_set("bootpart", devpart);
	env_set_hex("scriptaddr", SCRIPT_ADDR);
	env_set("bootfile", path);
	printf("Boot script: %s %s %s\n", env_get("bootdev"), devpart, path);
	return run_command("source 0x83000000", 0);
}

static int browse_scripts(struct blk_desc *desc, int part)
{
	char path[512] = "/", next[512];
	char names[12][FS_DIRENT_NAME_LEN];
	const char *items[16];
	bool dirs[12];
	unsigned int offset = 0;
	for (;;) {
		struct fs_dir_stream *stream;
		struct fs_dirent *e;
		unsigned int seen = 0, n = 0;
		bool more = false;
		int selected;
		if (fs_set_blk_dev_with_part(desc, part)) return -ENODEV;
		stream = fs_opendir(path);
		if (!stream) { fs_close(); return -ENOENT; }
		while ((e = fs_readdir(stream))) {
			size_t len = strlen(e->name);
			if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
			if (e->type != FS_DT_DIR && (e->type != FS_DT_REG ||
			    len < 4 || strcasecmp(e->name + len - 4, ".scr"))) continue;
			if (seen++ < offset) continue;
			if (n == 12) { more = true; break; }
			strlcpy(names[n], e->name, sizeof(names[n]));
			dirs[n] = e->type == FS_DT_DIR;
			items[n] = names[n];
			n++;
		}
		fs_closedir(stream);
		items[n] = more ? "Next page" : "First page";
		items[n + 1] = "Parent directory";
		items[n + 2] = "Back";
		selected = totoro_menu(path, items, n + 3);
		if (selected < 0 || selected == n + 2) return 0;
		if (selected == n) { offset = more ? offset + 12 : 0; continue; }
		offset = 0;
		if (selected == n + 1) {
			char *slash = strrchr(path, '/');
			if (slash == path) path[1] = 0;
			else if (slash) *slash = 0;
			continue;
		}
		if (snprintf(next, sizeof(next), "%s%s%s", path,
			     strcmp(path, "/") ? "/" : "", names[selected]) >= sizeof(next))
			return -ENAMETOOLONG;
		if (!dirs[selected]) return totoro_script(desc, part, next);
		strlcpy(path, next, sizeof(path));
	}
}

int totoro_script_menu(struct blk_desc *desc, int part)
{
	static const char *const items[] = {
		"Run /boot.scr", "Run /boot/boot.scr", "Browse .scr files",
		"List files", "Back"
	};
	int selected = totoro_menu("*** Boot script ***", items, ARRAY_SIZE(items));
	if (selected < 0 || selected == 4) return 0;
	if (selected == 0) return totoro_script(desc, part, "/boot.scr");
	if (selected == 1) return totoro_script(desc, part, "/boot/boot.scr");
	if (selected == 2) return browse_scripts(desc, part);
	if (fs_set_blk_dev_with_part(desc, part)) return -ENODEV;
	return fs_ls("/");
}

int totoro_sd_menu(void)
{
	struct mmc *mmc;
	struct blk_desc *desc;
	struct disk_partition info;
	char labels[14][32];
	const char *items[16];
	int map[14], n = 1, i, selected;
	if (run_command("sdinit", 0)) return -ENODEV;
	mmc = find_mmc_device(0);
	if (!mmc) return -ENODEV;
	desc = mmc_get_blk_desc(mmc);
	items[0] = "Whole card (no table)";
	map[0] = 0;
	for (i = 1; i <= 128 && n < 14; i++) {
		if (part_get_info(desc, i, &info)) continue;
		snprintf(labels[n], sizeof(labels[n]), "%d: %.20s", i, info.name);
		items[n] = labels[n];
		map[n++] = i;
	}
	items[n] = "Back";
	selected = totoro_menu("*** SD partition ***", items, n + 1);
	if (selected < 0 || selected == n) return 0;
	return totoro_script_menu(desc, map[selected]);
}

static int do_sdboot(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	struct mmc *mmc;
	ulong part;
	if (argc == 1) return totoro_sd_menu() ? CMD_RET_FAILURE : 0;
	if (strict_strtoul(argv[1], 16, &part) || part > 128) return CMD_RET_USAGE;
	if (run_command("sdinit", 0)) return CMD_RET_FAILURE;
	mmc = find_mmc_device(0);
	if (!mmc) return CMD_RET_FAILURE;
	return totoro_script(mmc_get_blk_desc(mmc), part, argc == 3 ? argv[2] : "/boot.scr")
		? CMD_RET_FAILURE : 0;
}
U_BOOT_CMD(sdboot, 3, 0, do_sdboot, "select SD partition and run a script",
	"[partition (hex) [path]] - defaults to /boot.scr");
