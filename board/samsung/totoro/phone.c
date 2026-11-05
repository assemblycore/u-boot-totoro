// SPDX-License-Identifier: GPL-2.0+
/* Native raw OneNAND/PIT reads and logical read/write through stock SBL.
 * PIT virtual-unit ranges are not BML remapping or STL logical sectors.
 * The native raw path stays read-only; all logical writes use Samsung FSR.
 */
#include <command.h>
#include <blk.h>
#include <dm.h>
#include <asm/global_data.h>
#include <console.h>
#include <env.h>
#include <g_dnl.h>
#include <usb.h>
#include <usb_mass_storage.h>
#include <watchdog.h>
#include <asm/io.h>
#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/mtd/onenand_regs.h>
#include <u-boot/crc.h>
#include "phone.h"
DECLARE_GLOBAL_DATA_PTR;

#define NAND_BASE ((u8 *)0x00400000)
#define PAGE_SIZE_NAND 4096U
#define BLOCK_SIZE_NAND 0x40000U
#define BLOCK_COUNT_NAND 2048U
#define MAX_PARTS 15
struct phone_part { char name[33]; u32 id, first, blocks; };
static struct phone_part parts[MAX_PARTS];
static unsigned int part_count;
static u8 pagebuf[PAGE_SIZE_NAND] __aligned(4);
static u32 cached_page = ~0U;

static int read_page(u32 page)
{
	u16 saved, status;
	unsigned int n;
	int ret = 0;
	if (page >= BLOCK_COUNT_NAND * 64) return -EINVAL;
	if (cached_page == page) return 0;
	cached_page = ~0U;
	saved = readw(NAND_BASE + ONENAND_REG_SYS_CFG1);
	writew(saved & ~ONENAND_SYS_CFG1_NO_ECC, NAND_BASE + ONENAND_REG_SYS_CFG1);
	writew(page / 64, NAND_BASE + ONENAND_REG_START_ADDRESS1);
	writew(0, NAND_BASE + ONENAND_REG_START_ADDRESS2);
	writew((page % 64) << 2, NAND_BASE + ONENAND_REG_START_ADDRESS8);
	writew(0x0800, NAND_BASE + ONENAND_REG_START_BUFFER); /* DataRAM0 */
	writew(0, NAND_BASE + ONENAND_REG_INTERRUPT);
	writew(ONENAND_CMD_READ, NAND_BASE + ONENAND_REG_COMMAND);
	for (n = 0; n < 10000; n++) {
		status = readw(NAND_BASE + ONENAND_REG_INTERRUPT);
		if ((status & (ONENAND_INT_MASTER | ONENAND_INT_READ)) ==
		    (ONENAND_INT_MASTER | ONENAND_INT_READ)) break;
		udelay(10);
	}
	if (n == 10000) ret = -ETIMEDOUT;
	else if (readw(NAND_BASE + ONENAND_REG_CTRL_STATUS) & ONENAND_CTRL_ERROR)
		ret = -EIO;
	for (n = 0; !ret && n < 4; n++)
		if (readw(NAND_BASE + ONENAND_REG_ECC_STATUS + n * 2) &
		    ONENAND_ECC_4BIT_UNCORRECTABLE) ret = -EBADMSG;
	if (!ret) {
		/* OneNAND buffer RAM is a 16-bit bus. */
		for (n = 0; n < PAGE_SIZE_NAND; n += 2)
			put_unaligned_le16(readw(NAND_BASE + ONENAND_DATARAM + n), pagebuf + n);
		cached_page = page;
	}
	writew(saved, NAND_BASE + ONENAND_REG_SYS_CFG1);
	return ret;
}

static int parse_pit(void)
{
	u32 count = get_unaligned_le32(pagebuf + 4), first = 0;
	unsigned int i, j, k;
	if (get_unaligned_le32(pagebuf) != 0x12349876 || !count || count > MAX_PARTS)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const u8 *e = pagebuf + 28 + 132 * i;
		u32 blocks = get_unaligned_le32(e + 24);
		if (get_unaligned_le32(e + 20) != 256 || !blocks ||
		    blocks > BLOCK_COUNT_NAND - first) return -EINVAL;
		for (j = 0; j < 32 && e[36 + j]; j++)
			if (!((e[36+j] >= 'a' && e[36+j] <= 'z') ||
			      (e[36+j] >= 'A' && e[36+j] <= 'Z') ||
			      (e[36+j] >= '0' && e[36+j] <= '9') ||
			      e[36+j] == '_' || e[36+j] == '-')) return -EINVAL;
		if (!j || j == 32) return -EINVAL;
		memcpy(parts[i].name, e + 36, j);
		parts[i].name[j] = 0;
		parts[i].id = get_unaligned_le32(e + 8);
		for (k = 0; k < i; k++)
			if (parts[k].id == parts[i].id || !strcmp(parts[k].name, parts[i].name))
				return -EINVAL;
		parts[i].first = first;
		parts[i].blocks = blocks;
		first += blocks;
	}
	part_count = count;
	return 0;
}

static int phone_init(void)
{
	u16 mid, did, size;
	u32 b, p;
	int ret;
	if (part_count) return 0;
	mid = readw(NAND_BASE + ONENAND_REG_MANUFACTURER_ID);
	did = readw(NAND_BASE + ONENAND_REG_DEVICE_ID);
	size = readw(NAND_BASE + ONENAND_REG_DATA_BUFFER_SIZE);
	printf("OneNAND: %04x:%04x buffer=%u\n", mid, did, size);
	/* Like onenand_check_features()/onenand_probe(): 4Gbit non-DDP
	 * uses a full 4KiB page even though F003 reports 2KiB BufferRAM.
	 * Also accept the old local QEMU model's 4KiB register value. */
	if (mid != 0xec || (did & 0x3f8) != 0x50 ||
	    (size != PAGE_SIZE_NAND / 2 && size != PAGE_SIZE_NAND)) {
		puts("Unsupported OneNAND geometry\n");
		return -ENODEV;
	}
	printf("Page=%u block=%u KiB\n", PAGE_SIZE_NAND, BLOCK_SIZE_NAND / 1024);
	/* Nominal systemdata VUN, then replacement area. Do not substitute
	 * a compiled-in partition table when no valid on-device PIT is found. */
	for (b = 17; b < BLOCK_COUNT_NAND; b = b == 17 ? 2004 : b + 1) {
		for (p = 0; p < 64; p++) {
			if (ctrlc()) return -EINTR;
			ret = read_page(b * 64 + p);
			if (ret == -ETIMEDOUT) { printf("OneNAND timeout block %u\n", b); return ret; }
			if (!ret && !parse_pit()) {
				printf("PIT: block %u page %u\n", b, p);
				return 0;
			}
		}
	}
	puts("PIT not found; BML mapping needed\n");
	return -ENOENT;
}

static int part_index(const char *name)
{
	unsigned int i;
	for (i = 0; i < part_count; i++) if (!strcmp(name, parts[i].name)) return i;
	return -ENOENT;
}

static int raw_read(const struct phone_part *part, u32 offset, u32 len, void *dest)
{
	u8 *out = dest;
	u32 size = part->blocks * BLOCK_SIZE_NAND, pos, n;
	int ret;
	if (offset > size || len > size - offset) return -EINVAL;
	pos = part->first * BLOCK_SIZE_NAND + offset;
	while (len) {
		u32 in_page = pos % PAGE_SIZE_NAND;
		ret = read_page(pos / PAGE_SIZE_NAND);
		if (ret) return ret;
		n = min(len, PAGE_SIZE_NAND - in_page);
		memcpy(out, pagebuf + in_page, n);
		out += n; pos += n; len -= n;
	}
	return 0;
}

static int stl_io(unsigned int idx, ulong addr, u32 sector, u32 count,
		  bool write, u32 *capacity);
static u8 disk_mbr[512];
static u32 disk_start[2], disk_size[2];
static int disk_idx[2];
static int disk_io(struct ums *ums, ulong start, lbaint_t count, void *buf, bool write)
{
	lbaint_t done = 0;
	u8 *p = buf;
	if (start > ums->num_sectors || count > ums->num_sectors - start) return -EINVAL;
	while (done < count) {
		unsigned int i;
		u32 n = 1;
		for (i = 0; i < 2; i++)
			if (start >= disk_start[i] && start - disk_start[i] < disk_size[i]) break;
		if (i < 2) {
			n = min_t(lbaint_t, count - done, disk_size[i] - (start - disk_start[i]));
			if (stl_io(disk_idx[i], (ulong)p, start - disk_start[i], n, write, NULL)) return -EIO;
		} else if (write) {
			/* The synthetic table is fixed; never translate edits into PIT writes. */
			if (start || memcmp(p, disk_mbr, 512)) return -EROFS;
		} else {
			memset(p, 0, 512);
			if (!start) memcpy(p, disk_mbr, 512);
		}
		start += n; done += n; p += n * 512;
	}
	return done;
}
static int phone_ums_read(struct ums *ums, ulong start, lbaint_t count, void *buf)
{
	if (ums->block_dev.devnum == -1) return disk_io(ums, start, count, buf, false);
	const struct phone_part *part = &parts[ums->block_dev.devnum];
	u32 sectors = ums->num_sectors;
	if (start > sectors || count > sectors - start) return -EINVAL;
	if (!ums->read_only)
		return stl_io(part - parts, (ulong)buf, start, count, false, NULL) ? -EIO : count;
	return raw_read(part, start * 512, count * 512, buf) ? -EIO : count;
}
static int phone_ums_write(struct ums *ums, ulong start, lbaint_t count, const void *buf)
{
	if (ums->block_dev.devnum == -1) return disk_io(ums, start, count, (void *)buf, true);
	if (ums->read_only) return -EROFS;
	if (start > ums->num_sectors || count > ums->num_sectors - start) return -EINVAL;
	return stl_io(ums->block_dev.devnum, (ulong)buf, start, count, true, NULL) ? -EIO : count;
}

/* idx == -1 exports all PIT entries as separate LUNs. */
static int phone_ums(int idx, bool logical)
{
	struct udevice *udc = NULL;
	struct ums luns[MAX_PARTS] = { 0 };
	int ret, i, n = 0;
	if (idx == -1) {
		memset(disk_mbr, 0, sizeof(disk_mbr));
		for (i = 0; i < 2; i++) {
			u8 *entry = disk_mbr + 446 + 16 * i;
			disk_idx[i] = part_index(i ? "userdata" : "system");
			if (disk_idx[i] < 0) return -ENOENT;
			ret = stl_io(disk_idx[i], 0, 0, 0, false, &disk_size[i]);
			if (ret) { printf("Cannot open %s: %d\n", parts[disk_idx[i]].name, ret); return ret; }
			disk_start[i] = i ? ALIGN(disk_start[0] + disk_size[0], 2048) : 2048;
			entry[4] = 0x83;
			memset(entry + 1, 0xff, 3); memset(entry + 5, 0xff, 3);
			put_unaligned_le32(disk_start[i], entry + 8);
			put_unaligned_le32(disk_size[i], entry + 12);
		}
		disk_mbr[510] = 0x55; disk_mbr[511] = 0xaa;
		luns[0].read_sector = phone_ums_read; luns[0].write_sector = phone_ums_write;
		luns[0].name = "Totoro internal"; luns[0].block_dev.devnum = -1;
		luns[0].num_sectors = disk_start[1] + disk_size[1];
		luns[0].block_dev.blksz = 512; luns[0].block_dev.lba = luns[0].num_sectors;
		n = 1;
		goto ready;
	}
	for (i = 0; i < part_count; i++) {
		struct ums *lun = &luns[n];
		bool translated;
		u32 sectors;
		if (idx >= 0 && idx != i) continue;
		translated = logical && parts[i].id >= 20 && parts[i].id <= 23;
		if (idx >= 0 && logical && !translated) return -EINVAL;
		sectors = parts[i].blocks * (BLOCK_SIZE_NAND / 512);
		if (translated) {
			ret = stl_io(i, 0, 0, 0, false, &sectors);
			if (ret) return ret;
		}
		lun->read_sector = phone_ums_read;
		lun->write_sector = phone_ums_write;
		lun->read_only = !translated;
		lun->name = parts[i].name;
		lun->num_sectors = sectors;
		lun->block_dev.devnum = i;
		lun->block_dev.blksz = 512;
		lun->block_dev.lba = sectors;
		printf("LUN %d %s: %s USB disk, %u sectors\n", n, lun->name,
		       translated ? "Read/write" : "Read-only raw", sectors);
		n++;
	}
ready:
	totoro_usb_release_console();
	puts("Eject on PC, then press Home\n");
	ret = udc_device_get_by_index(0, &udc);
	if (ret) return ret;
	ret = fsg_init(luns, n, udc);
	if (ret) goto put;
	g_dnl_clear_detach();
	ret = g_dnl_register("usb_dnl_ums");
	if (ret) goto put;
	while (!ctrlc()) {
		dm_usb_gadget_handle_interrupts(udc);
		ret = fsg_main_thread(NULL);
		if (ret) break;
		schedule();
	}
	g_dnl_unregister();
	g_dnl_clear_detach();
put:
	udc_device_put(udc);
	return ret;
}

static int phone_boot(unsigned int idx)
{
	u8 hdr[64];
	u32 len, hcrc, dcrc, entry;
	char cmd[40];
	int ret = raw_read(&parts[idx], 0, sizeof(hdr), hdr);
	if (ret) return ret;
	/* Only standalone ARM uImage, uncompressed, linked to 0x82000000. */
	if (get_unaligned_be32(hdr) != 0x27051956 || hdr[29] != 2 ||
	    hdr[30] != 1 || hdr[31] || get_unaligned_be32(hdr + 16) != 0x82000000) {
		puts("Expected ARM standalone uImage\nload address 0x82000000\n");
		return -ENOEXEC;
	}
	hcrc = get_unaligned_be32(hdr + 4);
	put_unaligned_be32(0, hdr + 4);
	len = get_unaligned_be32(hdr + 12);
	entry = get_unaligned_be32(hdr + 20);
	dcrc = get_unaligned_be32(hdr + 24);
	if (crc32(0, hdr, sizeof(hdr)) != hcrc || !len || len > 0x01000000 ||
	    entry < 0x82000000 || entry - 0x82000000 >= len || (entry & 3)) return -EINVAL;
	ret = raw_read(&parts[idx], sizeof(hdr), len, (void *)0x82000000);
	if (ret) return ret;
	if (crc32(0, (void *)0x82000000, len) != dcrc) return -EBADMSG;
	snprintf(cmd, sizeof(cmd), "go 0x%08x", entry);
	return run_command(cmd, 0);
}

static ulong phone_blk_read(struct udevice *dev, lbaint_t start,
			    lbaint_t count, void *buf)
{
	unsigned int idx = *(unsigned int *)dev_get_plat(dev);
	return stl_io(idx, (ulong)buf, start, count, false, NULL) ? 0 : count;
}

static ulong phone_blk_write(struct udevice *dev, lbaint_t start,
			     lbaint_t count, const void *buf)
{
	unsigned int idx = *(unsigned int *)dev_get_plat(dev);
	return stl_io(idx, (ulong)buf, start, count, true, NULL) ? 0 : count;
}

static const struct blk_ops phone_blk_ops = {
	.read = phone_blk_read, .write = phone_blk_write,
};
U_BOOT_DRIVER(totoro_stl_blk) = {
	.name = "totoro_stl_blk", .id = UCLASS_BLK,
	.ops = &phone_blk_ops, .plat_auto = sizeof(unsigned int),
};

static int phone_block(unsigned int idx, struct blk_desc **desc)
{
	static struct udevice *devices[MAX_PARTS];
	u32 sectors;
	int ret = stl_io(idx, 0, 0, 0, false, &sectors);
	if (ret) return ret;
	if (!devices[idx]) {
		ret = blk_create_devicef(gd->dm_root, "totoro_stl_blk", parts[idx].name,
			UCLASS_MTD, parts[idx].id, 512, sectors, &devices[idx]);
		if (ret) return ret;
		*(unsigned int *)dev_get_plat(devices[idx]) = idx;
	}
	*desc = dev_get_uclass_plat(devices[idx]);
	return 0;
}

int totoro_phone_menu(bool boot)
{
	const char *names[MAX_PARTS + 1];
	unsigned int i, map[MAX_PARTS], count = 0;
	int idx, ret = phone_init();
	struct blk_desc *desc;
	if (ret) return ret;
	if (!boot) return phone_ums(-1, true);
	idx = part_index("system");
	if (idx < 0) return idx;
	ret = phone_block(idx, &desc);
	if (!ret) ret = totoro_script(desc, 0, "/boot.scr");
	return ret;
	for (i = 0; i < part_count; i++) {
		if (parts[i].id < 20 || parts[i].id > 23) continue;
		map[count] = i;
		names[count++] = parts[i].name;
	}
	names[count] = "Back";
	idx = totoro_menu(boot ? "*** Internal Boot ***" : "*** Internal USB R/W ***",
			 names, count + 1);
	if (idx < 0 || idx == count) return 0;
	idx = map[idx];
	ret = phone_block(idx, &desc);
	if (!ret) ret = totoro_script_menu(desc, 0);
	if (ret) printf("Internal operation error: %d\n", ret);
	return ret;
}

/* Use only a positively identified resident SBL. Its FSR heap remains above
 * U-Boot's RAM window. No firmware bytes are included or patched. */
static int stl_io(unsigned int idx, ulong addr, u32 sector, u32 count,
		  bool write, u32 *capacity)
{
	static u32 open, read, program, close, ioctl;
	static u32 session_info[4][4];
	static bool opened[4];
	u32 crc, ret, wb[2] = { 0 }, returned = 0;
	u32 id = parts[idx].id;
	u32 *info;
	if (id < 20 || id > 23 || (!capacity && !count)) return -EINVAL;
	info = session_info[id - 20];
	if (!open) {
		crc = crc32(0, (void *)0x96800000, 0x80000);
		if (crc == 0xb3fd4a22) {
			open = 0x9684c6cc; read = 0x9684d744; close = 0x9684d164;
			program = 0x9684dccc;
			ioctl = 0x9684ef0c;
		} else if (crc == 0xa5d48cad) {
			open = 0x9684d0e8; read = 0x9684e160; close = 0x9684db80;
			program = 0x9684e6e8;
			ioctl = 0x9684f928;
		} else {
			printf("Unsupported resident SBL CRC=%08x\n", crc);
			return -ENOTSUPP;
		}
	}
	/* Keep the FSR mapping alive across USB requests. Repeated Open/Close
	 * exhausts the small resident SBL heap on a PC's filesystem probe. */
	if (!opened[id - 20]) {
		memset(info, 0, sizeof(session_info[0]));
		/* Reopen SBL's logo session once to obtain its logical capacity.
		 * Keep RW mode across reads/writes rather than toggling NAND locks. */
		ret = ((u32 (*)(u32, u32, u32 *, u32))open)(0, id, info, 0x80);
		if (ret == 0x80040002) {
			ret = ((u32 (*)(u32, u32))close)(0, id);
			if (!ret)
				ret = ((u32 (*)(u32, u32, u32 *, u32))open)(0, id, info, 0x80);
		}
		if (ret) { printf("STL open: %08x\n", ret); return -EIO; }
		if (!info[2] || info[2] > parts[idx].blocks * 512) {
			((u32 (*)(u32, u32))close)(0, id);
			return -EINVAL;
		}
		opened[id - 20] = true;
		printf("STL session: %s, %u sectors\n", parts[idx].name, info[2]);
	}
	if (!info[2] || info[2] > parts[idx].blocks * 512 ||
	    (!capacity && (sector >= info[2] || count > info[2] - sector))) ret = 1;
	else if (capacity) { *capacity = info[2]; ret = 0; }
	else ret = ((u32 (*)(u32, u32, u32, u32, void *, u32))(write ? program : read))
		(0, id, sector, count, (void *)addr, 0);
	/* FSR_STL_IOCTL_GET_WB_INFO (0xb6) / FLUSH_WB (0xc6).
	 * Commit every write before acknowledging it, without reopening STL.
	 * The input to FLUSH_WB is the used-block count returned by GET_WB_INFO. */
	if (!ret && write && !capacity) {
		typedef u32 (*ctl_fn)(u32, u32, u32, void *, u32, void *, u32, u32 *);
		ret = ((ctl_fn)ioctl)(0, id, 0xb6, NULL, 0, wb, sizeof(wb), &returned);
		/* Both supported SBLs return STL_ERROR here when the partition
		 * has no optional write buffer (NULL at context + 0x64).
		 * Its writes already go directly through STL to flash. */
		if (ret == 0x80000000) { ret = 0; returned = sizeof(wb); }
		if (!ret && returned != sizeof(wb)) ret = 1;
		if (!ret && wb[1])
			ret = ((ctl_fn)ioctl)(0, id, 0xc6, &wb[1], sizeof(wb[1]), NULL, 0, &returned);
	}
	cached_page = ~0U;
	if (ret) {
		printf("STL I/O/flush=%08x\n", ret);
		return -EIO;
	}
	return 0;
}

static int do_phone(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	int ret, idx;
	unsigned int i;
	ulong addr, offset, len;
	if (argc < 2) return CMD_RET_USAGE;
	ret = phone_init();
	if (ret) return CMD_RET_FAILURE;
	if (argc == 2 && !strcmp(argv[1], "part")) {
		puts("Phone PIT: ID name size(KiB)\n");
		for (i = 0; i < part_count; i++)
			printf("%2u %-13s %7u\n", parts[i].id, parts[i].name, parts[i].blocks * 256);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "ums-all"))
		return phone_ums(-1, true) ? CMD_RET_FAILURE : 0;
	if (argc < 3) return CMD_RET_USAGE;
	idx = part_index(argv[2]);
	if (idx < 0) { puts("Unknown phone partition\n"); return CMD_RET_FAILURE; }
	if (argc == 6 && (!strcmp(argv[1], "read") || !strcmp(argv[1], "write"))) {
		if (strict_strtoul(argv[3], 16, &addr) || strict_strtoul(argv[4], 16, &offset) ||
		    strict_strtoul(argv[5], 16, &len) || addr < 0x82000000 || addr >= 0x83000000 ||
		    !len || len > (0x83000000 - addr) / 512) return CMD_RET_USAGE;
		bool write = !strcmp(argv[1], "write");
		ret = stl_io(idx, addr, offset, len, write, NULL);
		if (!ret) {
			env_set_hex("filesize", len * 512);
			printf("%s %lu logical sectors\n", write ? "Wrote" : "Read", len);
		}
	} else if ((argc == 3 || argc == 4) && !strcmp(argv[1], "script")) {
		struct blk_desc *desc;
		ret = phone_block(idx, &desc);
		if (!ret) ret = totoro_script(desc, 0, argc == 4 ? argv[3] : "/boot.scr");
	} else if (argc == 3 && !strcmp(argv[1], "bind")) {
		struct blk_desc *desc;
		ret = phone_block(idx, &desc);
		if (!ret) printf("STL %s: mtd %x:0\n", parts[idx].name, parts[idx].id);
	} else if (argc == 3 && !strcmp(argv[1], "boot")) ret = phone_boot(idx);
	else if (argc == 3 && !strcmp(argv[1], "ums-raw")) ret = phone_ums(idx, false);
	else if (argc == 3 && !strcmp(argv[1], "ums")) ret = phone_ums(idx, true);
	else if (argc == 6 && !strcmp(argv[1], "read-raw")) {
		if (strict_strtoul(argv[3], 16, &addr) || strict_strtoul(argv[4], 16, &offset) ||
		    strict_strtoul(argv[5], 16, &len) || addr < 0x82000000 || addr >= 0x83000000 ||
		    !len || len > 0x83000000 - addr) return CMD_RET_USAGE;
		ret = raw_read(&parts[idx], offset, len, (void *)addr);
		if (!ret) { env_set_hex("filesize", len); printf("Read %lu raw bytes\n", len); }
	} else return CMD_RET_USAGE;
	if (ret) printf("Phone error: %d\n", ret);
	return ret ? CMD_RET_FAILURE : 0;
}
U_BOOT_CMD(phone, 6, 0, do_phone, "internal OneNAND/PIT access",
	"part\nphone read/write <name> <RAM address> <sector> <count> (hex, logical STL)\n"
	"phone ums <name> - logical read/write USB disk\n"
	"phone ums-all - all PIT entries, STL R/W and raw read-only\n"
	"phone bind <name> - expose STL as mtd <PIT ID>:0\n"
	"phone script <name> [path] - run boot.scr from a filesystem\n"
	"phone read-raw <name> <RAM address> <offset> <bytes> (hex)\n"
	"phone ums-raw <name> - read-only raw range, no BML/STL translation\n"
	"phone boot <name> - ARM standalone uImage at nominal PIT range");
