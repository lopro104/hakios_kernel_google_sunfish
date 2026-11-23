/* Part of the loader (included by loader.c, not compiled alone): the DSU image found on the phone, and read through its extents. */
/* Find the extents of the DSU partition WANT (into OUT, *OUT_N, *OUT_BYTES); 1 if there is exactly one plausible copy of the metadata (older ones a rename left are ignored when they agree, extent by extent). */
static int
dsu_find_named(const char *want, struct dsu_ext *out, unsigned *out_n, uint64_t *out_bytes, efi_block_io **userdata_out)
{
	efi_block_io *meta = partition_named("metadata"), *data = partition_named("userdata");
	uint64_t bs, meta_bytes, chunk = sizeof(ufs_buffer), found = 0;
	uint64_t first_bytes = 0, first_extents = 0;
	static uint8_t first_table[sizeof(dsu_extent)];  /* (one at a time: the loader runs on one core) */         /* (the first copy's extents, each later one compared with them) */

	if (meta == NULL || data == NULL) {
		print("dsu: no %s partition\n", meta == NULL ? "metadata" : "userdata");
		return 0;
	}
	bs = meta->media->block_size;
	meta_bytes = (meta->media->last_block + 1) * bs;
	for (uint64_t at = 0; at < meta_bytes; at += chunk - 64 * 1024) {
		uint64_t n = meta_bytes - at < chunk ? meta_bytes - at : chunk;

		n -= n % bs;
		if (EFI_ERROR(meta->read_blocks(meta, meta->media->media_id, at / bs, n, ufs_buffer))) {
			print("dsu: metadata read failed at %x\n", at);
			return 0;
		}
		/* only blocks in the first part of the window start a copy: the last 64 KB are read again next time */
		for (uint64_t b = 0; b + 4096 <= n && (b < chunk - 64 * 1024 || at + n == meta_bytes); b += 4096) {
			if (*(const uint32_t *)(ufs_buffer + b) != LP_GEOMETRY_MAGIC ||
			    !dsu_parse(ufs_buffer + b, n - b, (data->media->last_block + 1) * data->media->block_size / 512, want, out, out_n, out_bytes)) {
				continue;
			}
			print("dsu: lp_metadata at %x of metadata: %s %d bytes in %d extents, the first at sector %x\n",
			    at + b, want, *out_bytes, (uint64_t)*out_n, out[0].sector);
			if (found++ == 0) {
				first_bytes = *out_bytes, first_extents = *out_n;
				memcpy(first_table, out, *out_n * sizeof(out[0]));
			} else if (*out_bytes != first_bytes || *out_n != first_extents ||
			    memcmp(first_table, out, *out_n * sizeof(out[0])) != 0) {
				print("dsu: two different copies of the metadata: not choosing\n");
				return 0;
			}
		}
	}
	if (found == 0) {
		print("dsu: no lp_metadata with a %s in the metadata partition\n", want);
		return 0;
	}
	*userdata_out = data;
	return 1;
}

/* system_gsi: the Darwin system volume */
static int
dsu_find(efi_block_io **userdata_out)
{
	return dsu_find_named("system_gsi", dsu_extent, &dsu_extents, &dsu_bytes, userdata_out);
}

/* userdata_gsi: the DSU's data image (none unless the install made one) */
static int
dsud_find(efi_block_io **userdata_out)
{
	return dsu_find_named("userdata_gsi", dsud_extent, &dsud_extents, &dsud_bytes, userdata_out);
}

/*
 * `n` bytes of a DSU image (its extents EXT, N_EXT of them, IMAGE_BYTES in all) from byte `offset` (both whole
 * blocks), through its extents: read into DEST, or with WRITE set written from it.
 */
static efi_status
dsu_io(const struct dsu_ext *ext, unsigned n_ext, uint64_t image_bytes, efi_block_io *data, uint64_t offset, uint64_t n,
    uint8_t *dest, int write)
{
	uint64_t bs = data->media->block_size, image_at = 0;

	if (offset > image_bytes || n > image_bytes - offset || offset % bs || n % bs) {
		return 0x8000000000000002ULL;   /* EFI_INVALID_PARAMETER */
	}
	for (unsigned x = 0; x < n_ext && n > 0; x++) {
		uint64_t len = ext[x].count * 512;

		while (n > 0 && offset < image_at + len) {
			uint64_t at = ext[x].sector * 512 + (offset - image_at), piece = image_at + len - offset;
			efi_status st;

			if (piece > n) {
				piece = n;
			}
			if (at % bs) {
				return 0x8000000000000002ULL;
			}
			st = write ? data->write_blocks(data, data->media->media_id, at / bs, piece, dest)
			    : data->read_blocks(data, data->media->media_id, at / bs, piece, dest);
			if (EFI_ERROR(st)) {
				return st;
			}
			dest += piece;
			offset += piece;
			n -= piece;
		}
		image_at += len;
	}
	return n == 0 ? 0 : 0x8000000000000002ULL;
}

static efi_status
dsu_read(efi_block_io *data, uint64_t offset, uint64_t n, uint8_t *dest)
{
	return dsu_io(dsu_extent, dsu_extents, dsu_bytes, data, offset, n, dest, 0);
}
