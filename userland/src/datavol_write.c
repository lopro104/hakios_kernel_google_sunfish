/*
 * datavol_write IMAGE.simg [check|force]: writes the data volume (an empty HFS Plus volume, made on the PC by
 * tools/mkdatavol.py) into the phone's darwin partition, from inside Darwin.
 *
 * Why from inside: the bootloader's fastboot refuses to flash `darwin` ("Not allowed to flash"), and the kernel
 * already takes writes to /dev/rufs_darwin from block 2042368 on (ufs_write=1, drivers/storage/hakios.ufs.c), which is
 * how var_store keeps its archive. The image is an Android sparse file (53 KB: only the blocks the volume uses are in
 * it); what is DONT_CARE is not touched, what is RAW or FILL is written, whole blocks, and read back.
 *
 *   check   parse the image and say what it would write; write nothing
 *   force   write even though a volume is already there (which empties it: everything in /private/var kept there goes)
 *
 * Safety, in this order: nothing is written before DATA_FIRST (8 GiB: the firmware container, mounted on /fw, is at
 * block 2042368, 85 MB long, and the volume starts after it); a volume that is already there is left alone unless
 * "force"; every block written is read back and compared, and the result says so.
 */
#include "mini.h"

#define BLOCK        4096
#define DATA_FIRST   2097152LL          /* darwin's block where the volume begins (drivers/storage/hakios.ufs.c UFS_DATA_FIRST) */
#define DEVICE       "/dev/rufs_darwin"
#define MAX_IMAGE    (4u << 20)
#define STEP         (1u << 20)         /* bytes a write or a read: the loader's buffer takes more, this is plenty */

#define CHUNK_RAW        0xCAC1
#define CHUNK_FILL       0xCAC2
#define CHUNK_DONT_CARE  0xCAC3
#define CHUNK_CRC        0xCAC4
#define SPARSE_MAGIC     0xED26FF3Au

static int
sysctl_set_int(const char *name, int value)
{
	return sysctlbyname(name, NULL, NULL, &value, sizeof(value));
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t
le16(const uint8_t *p)
{
	return p[0] | (p[1] << 8);
}

static int fd = -1;
static uint8_t *scratch;                /* STEP bytes: a write's data, and what is read back */
static uint8_t *readback;

/* One run of blocks: `data` is the bytes (RAW) or a 4-byte fill value (FILL, then data is NULL and fill says which). */
static int
put_blocks(long long first, uint64_t count, const uint8_t *data, uint32_t fill, int check_only, uint64_t *written)
{
	uint64_t done = 0;

	if (first < DATA_FIRST) {
		printf("datavol_write: block %lld is before the data volume (%lld): refused, nothing written for it\n", first,
		    DATA_FIRST);
		return 0;
	}
	while (done < count) {
		uint64_t blocks = count - done;
		size_t bytes;

		if (blocks > STEP / BLOCK) {
			blocks = STEP / BLOCK;
		}
		bytes = (size_t)(blocks * BLOCK);
		if (data != NULL) {
			memcpy(scratch, data + done * BLOCK, bytes);
		} else {
			for (size_t i = 0; i < bytes; i += 4) {
				scratch[i] = (uint8_t)fill;
				scratch[i + 1] = (uint8_t)(fill >> 8);
				scratch[i + 2] = (uint8_t)(fill >> 16);
				scratch[i + 3] = (uint8_t)(fill >> 24);
			}
		}
		if (!check_only) {
			off_t where = (off_t)(first + (long long)done) * BLOCK;
			ssize_t n = pwrite(fd, scratch, bytes, where);

			if (n != (ssize_t)bytes) {
				printf("datavol_write: writing %zu bytes at block %lld failed: %s\n", bytes,
				    first + (long long)done, strerror(errno));
				return 0;
			}
			n = pread(fd, readback, bytes, where);
			if (n != (ssize_t)bytes || memcmp(readback, scratch, bytes) != 0) {
				printf("datavol_write: block %lld read back DIFFERENT (%zd of %zu bytes)\n", first + (long long)done, n,
				    bytes);
				return 0;
			}
		}
		*written += blocks;
		done += blocks;
	}
	return 1;
}

/* Is there already an HFS Plus volume where ours would go? (the volume header sits 1024 bytes in) */
static int
volume_present(void)
{
	uint8_t head[BLOCK];
	ssize_t n = pread(fd, head, BLOCK, DATA_FIRST * BLOCK);

	if (n != BLOCK) {
		return -1;
	}
	return (head[1024] == 'H' && (head[1025] == '+' || head[1025] == 'X')) ? 1 : 0;
}

int
main(int argc, char **argv, char **envp)
{
	const char *path = argc > 1 ? argv[1] : "/redwood/share/datavol.simg";
	const char *mode = argc > 2 ? argv[2] : "";
	int check_only = strcmp(mode, "check") == 0, force = strcmp(mode, "force") == 0;
	uint8_t *img;
	int image_fd;
	ssize_t size;
	uint64_t blocks_total, written = 0, raw = 0, fills = 0, dont_care = 0;
	uint32_t chunks, file_hdr, chunk_hdr;
	long long at = 0;
	size_t pos;
	int claimed = 0, ok = 1;

	(void)envp;
	img = mmap(NULL, MAX_IMAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	scratch = mmap(NULL, STEP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	readback = mmap(NULL, STEP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (img == MAP_FAILED || scratch == MAP_FAILED || readback == MAP_FAILED) {
		printf("datavol_write: no memory\n");
		return 1;
	}
	image_fd = open(path, O_RDONLY, 0);
	if (image_fd < 0) {
		printf("datavol_write: %s: %s\n", path, strerror(errno));
		return 1;
	}
	size = read(image_fd, img, MAX_IMAGE);
	close(image_fd);
	if (size < 28 || le32(img) != SPARSE_MAGIC) {
		printf("datavol_write: %s is not an Android sparse image\n", path);
		return 1;
	}
	file_hdr = le16(img + 8);
	chunk_hdr = le16(img + 10);
	if (le32(img + 12) != BLOCK || file_hdr < 28 || chunk_hdr < 12) {
		printf("datavol_write: unexpected sparse header (block size %u)\n", le32(img + 12));
		return 1;
	}
	blocks_total = le32(img + 16);
	chunks = le32(img + 20);

	if (!check_only) {
		fd = open(DEVICE, O_RDWR, 0);
		if (fd < 0) {
			printf("datavol_write: %s: %s (the kernel takes writes with ufs_write=1)\n", DEVICE, strerror(errno));
			return 1;
		}
		if (!force) {
			int present = volume_present();

			if (present != 0) {
				printf("datavol_write: %s: not writing (use force to write over it, which empties it)\n",
				    present > 0 ? "a volume is already at the data start" : "the data start could not be read");
				close(fd);
				return present > 0 ? 0 : 1;
			}
		}
		if (sysctl_set_int("kern.hakios.var_busy", 1) != 0) {
			printf("datavol_write: the store is claimed by another program (%s)\n", strerror(errno));
			close(fd);
			return 1;
		}
		claimed = 1;
	}

	pos = file_hdr;
	for (uint32_t i = 0; i < chunks && ok; i++) {
		uint32_t type, cblocks, total;

		if (pos + chunk_hdr > (size_t)size) {
			printf("datavol_write: the image ends inside chunk %u\n", i);
			ok = 0;
			break;
		}
		type = le16(img + pos);
		cblocks = le32(img + pos + 4);
		total = le32(img + pos + 8);
		if (pos + total > (size_t)size) {
			printf("datavol_write: chunk %u is longer than the image\n", i);
			ok = 0;
			break;
		}
		if (type == CHUNK_RAW) {
			raw += cblocks;
			ok = put_blocks(at, cblocks, img + pos + chunk_hdr, 0, check_only, &written);
		} else if (type == CHUNK_FILL) {
			fills += cblocks;
			ok = put_blocks(at, cblocks, NULL, le32(img + pos + chunk_hdr), check_only, &written);
		} else if (type == CHUNK_DONT_CARE) {
			dont_care += cblocks;
		} else if (type != CHUNK_CRC) {
			printf("datavol_write: chunk %u has an unknown type %x\n", i, type);
			ok = 0;
		}
		at += cblocks;
		pos += total;
	}
	if (ok && (uint64_t)at != blocks_total) {
		printf("datavol_write: the chunks cover %lld blocks, the header says %llu\n", at, (unsigned long long)blocks_total);
		ok = 0;
	}
	if (claimed) {
		(void)sysctl_set_int("kern.hakios.var_busy", 0);
	}
	if (fd >= 0) {
		close(fd);
	}
	printf("datavol_write: %s: %llu blocks %s (%llu raw, %llu fill; %llu left alone), image covers %lld blocks from block 0: %s\n",
	    path, (unsigned long long)written, check_only ? "would be written" : "written and read back the same",
	    (unsigned long long)raw, (unsigned long long)fills, (unsigned long long)dont_care, at, ok ? "OK" : "FAILED");
	return ok ? 0 : 1;
}
