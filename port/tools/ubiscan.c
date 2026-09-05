// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal read-only scanner for locating concatenated UBI devices on raw MTD.
 * It reads only EC/VID headers and the first volume-table record.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>
#include <unistd.h>

#define PEB_SIZE 0x20000
#define EC_MAGIC 0x55424923U
#define VID_MAGIC 0x55424921U
#define LAYOUT_VOL_ID 0x7fffefffU
#define VTBL_RECORD_SIZE 172

static uint16_t be16(const unsigned char *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const unsigned char *p)
{
	return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static int read_at(int fd, void *buf, size_t len, off_t off)
{
	ssize_t got = pread(fd, buf, len, off);

	if (got < 0) {
		fprintf(stderr, "pread at 0x%llx: %s\n",
			(unsigned long long)off, strerror(errno));
		return -1;
	}
	return got == (ssize_t)len ? 0 : -1;
}

int main(int argc, char **argv)
{
	unsigned char ec[64], vid[64], rec[VTBL_RECORD_SIZE];
	struct mtd_info_user mtd;
	off_t size;
	uint32_t run_seq = 0;
	unsigned long long run_start = 0;
	int run_active = 0;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s /dev/mtdN\n", argv[0]);
		return 2;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	if (ioctl(fd, MEMGETINFO, &mtd)) {
		perror("MEMGETINFO");
		close(fd);
		return 1;
	}
	size = mtd.size;
	printf("%s: %llu bytes, %llu PEBs\n", argv[1],
	       (unsigned long long)size,
	       (unsigned long long)(size / PEB_SIZE));

	for (off_t off = 0; off + PEB_SIZE <= size; off += PEB_SIZE) {
		uint32_t data_off, image_seq, lnum, vid_off, vol_id;
		unsigned long long peb = off / PEB_SIZE;
		uint64_t sqnum;

		if (read_at(fd, ec, sizeof(ec), off))
			break;
		if (be32(ec) != EC_MAGIC) {
			if (run_active) {
				printf("EC run PEB=%llu-%llu offset=0x%08llx-0x%08llx image_seq=%u\n",
				       run_start, peb - 1,
				       run_start * PEB_SIZE, peb * PEB_SIZE,
				       run_seq);
				run_active = 0;
			}
			continue;
		}

		vid_off = be32(ec + 16);
		data_off = be32(ec + 20);
		image_seq = be32(ec + 24);
		if (!run_active) {
			run_start = peb;
			run_seq = image_seq;
			run_active = 1;
		} else if (image_seq != run_seq) {
			printf("EC run PEB=%llu-%llu offset=0x%08llx-0x%08llx image_seq=%u\n",
			       run_start, peb - 1,
			       run_start * PEB_SIZE, peb * PEB_SIZE,
			       run_seq);
			run_start = peb;
			run_seq = image_seq;
		}

		if (vid_off > PEB_SIZE - sizeof(vid) ||
		    read_at(fd, vid, sizeof(vid), off + vid_off))
			continue;
		if (be32(vid) != VID_MAGIC)
			continue;

		vol_id = be32(vid + 8);
		lnum = be32(vid + 12);
		sqnum = be64(vid + 40);
		if (vol_id != LAYOUT_VOL_ID)
			continue;

		printf("layout PEB=%llu offset=0x%08llx lnum=%u sqnum=%llu image_seq=%u",
		       (unsigned long long)(off / PEB_SIZE),
		       (unsigned long long)off, lnum,
		       (unsigned long long)sqnum, image_seq);

		if (data_off <= PEB_SIZE - sizeof(rec) &&
		    !read_at(fd, rec, sizeof(rec), off + data_off)) {
			uint16_t name_len = be16(rec + 14);
			uint32_t reserved = be32(rec);

			if (name_len > 127)
				name_len = 127;
			printf(" record0={name=\"%.*s\", reserved_pebs=%u}",
			       name_len, rec + 16, reserved);
		}
		putchar('\n');
	}
	if (run_active) {
		unsigned long long end_peb = size / PEB_SIZE;

		printf("EC run PEB=%llu-%llu offset=0x%08llx-0x%08llx image_seq=%u\n",
		       run_start, end_peb - 1,
		       run_start * PEB_SIZE, end_peb * PEB_SIZE,
		       run_seq);
	}

	close(fd);
	return 0;
}
