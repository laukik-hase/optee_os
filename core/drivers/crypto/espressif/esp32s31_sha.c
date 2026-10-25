// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2026 Espressif Systems (Shanghai) Co. Ltd.
 *
 * ESP32-S31 SHA accelerator
 */

#include <assert.h>
#include <drvcrypt.h>
#include <drvcrypt_hash.h>
#include <io.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <malloc.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>
#include <string.h>
#include <tee_api_types.h>
#include <trace.h>
#include <utee_defines.h>
#include <util.h>

#include "esp32s31_crypto.h"

/* SHA registers */
#define SHA_MODE			0x00
#define SHA_START			0x10
#define SHA_CONTINUE			0x14
#define SHA_BUSY			0x18
#define SHA_H_MEM			0x40
#define SHA_M_MEM			0x80
#define SHA_SIZE			0x100

#define SHA_MODE_SHA1			0
#define SHA_MODE_SHA224			1
#define SHA_MODE_SHA256			2
#define SHA_MODE_SHA384			3
#define SHA_MODE_SHA512			4

#define SHA_MAX_BLOCK_SIZE		128
#define SHA_MAX_STATE_SIZE		64
#define SHA_TIMEOUT_US			1000

struct esp32s31_sha_ctx {
	struct crypto_hash_ctx hash_ctx;
	uint32_t mode;
	size_t blk_size;
	size_t dgt_size;
	size_t state_size;
	bool started;
	uint64_t total[2];
	uint8_t buf[SHA_MAX_BLOCK_SIZE];
	uint32_t state[SHA_MAX_STATE_SIZE / sizeof(uint32_t)];
};

static struct esp32s31_sha {
	struct io_pa_va base;
	struct mutex lock;
} sha = {
	.base.pa = ESP32S31_SHA_BASE,
	.lock = MUTEX_INITIALIZER,
};

static const struct crypto_hash_ops esp32s31_sha_ops;

static struct esp32s31_sha_ctx *to_sha_ctx(struct crypto_hash_ctx *ctx)
{
	assert(ctx && ctx->ops == &esp32s31_sha_ops);

	return container_of(ctx, struct esp32s31_sha_ctx, hash_ctx);
}

static TEE_Result sha_wait_idle(void)
{
	uint32_t busy = 0;

	if (IO_READ32_POLL_TIMEOUT(sha.base.va + SHA_BUSY, busy, !busy, 0,
				   SHA_TIMEOUT_US))
		return TEE_ERROR_BUSY;

	return TEE_SUCCESS;
}

/* Hash whole blocks, resuming from and saving back the context state */
static TEE_Result sha_process(struct esp32s31_sha_ctx *hctx,
			      const uint8_t *data, size_t nblocks)
{
	TEE_Result res = TEE_SUCCESS;
	uint32_t word = 0;
	size_t i = 0;

	mutex_lock(&sha.lock);

	io_write32(sha.base.va + SHA_MODE, hctx->mode);

	if (hctx->started) {
		for (i = 0; i < hctx->state_size / sizeof(uint32_t); i++)
			io_write32(sha.base.va + SHA_H_MEM + i * 4,
				   hctx->state[i]);
	}

	for (; nblocks; nblocks--, data += hctx->blk_size) {
		for (i = 0; i < hctx->blk_size; i += sizeof(word)) {
			memcpy(&word, data + i, sizeof(word));
			io_write32(sha.base.va + SHA_M_MEM + i, word);
		}

		if (hctx->started)
			io_write32(sha.base.va + SHA_CONTINUE, 1);
		else
			io_write32(sha.base.va + SHA_START, 1);
		hctx->started = true;

		res = sha_wait_idle();
		if (res)
			goto out;
	}

	for (i = 0; i < hctx->state_size / sizeof(uint32_t); i++)
		hctx->state[i] = io_read32(sha.base.va + SHA_H_MEM + i * 4);

out:
	mutex_unlock(&sha.lock);

	return res;
}

static TEE_Result esp32s31_sha_init(struct crypto_hash_ctx *ctx)
{
	struct esp32s31_sha_ctx *hctx = to_sha_ctx(ctx);

	hctx->started = false;
	hctx->total[0] = 0;
	hctx->total[1] = 0;
	memset(hctx->buf, 0, sizeof(hctx->buf));
	memset(hctx->state, 0, sizeof(hctx->state));

	return TEE_SUCCESS;
}

static TEE_Result esp32s31_sha_update(struct crypto_hash_ctx *ctx,
				      const uint8_t *data, size_t len)
{
	struct esp32s31_sha_ctx *hctx = to_sha_ctx(ctx);
	TEE_Result res = TEE_SUCCESS;
	size_t left = 0;
	size_t fill = 0;
	size_t nblocks = 0;

	if (!len)
		return TEE_SUCCESS;
	if (!data)
		return TEE_ERROR_BAD_PARAMETERS;

	left = hctx->total[0] & (hctx->blk_size - 1);
	fill = hctx->blk_size - left;

	hctx->total[0] += len;
	if (hctx->total[0] < len)
		hctx->total[1]++;

	if (left && len >= fill) {
		memcpy(hctx->buf + left, data, fill);
		res = sha_process(hctx, hctx->buf, 1);
		if (res)
			return res;

		data += fill;
		len -= fill;
		left = 0;
	}

	nblocks = len / hctx->blk_size;
	if (nblocks) {
		res = sha_process(hctx, data, nblocks);
		if (res)
			return res;

		data += nblocks * hctx->blk_size;
		len -= nblocks * hctx->blk_size;
	}

	if (len)
		memcpy(hctx->buf + left, data, len);

	return TEE_SUCCESS;
}

static TEE_Result esp32s31_sha_final(struct crypto_hash_ctx *ctx,
				     uint8_t *digest, size_t len)
{
	struct esp32s31_sha_ctx *hctx = to_sha_ctx(ctx);
	uint8_t pad[SHA_MAX_BLOCK_SIZE] = { 0x80 };
	uint8_t bits[16] = { };
	size_t len_size = hctx->blk_size / 8;
	size_t last = 0;
	size_t padn = 0;
	TEE_Result res = TEE_SUCCESS;

	/* Message length in bits, big endian, 64 or 128 bits wide */
	put_be64(bits, (hctx->total[0] >> 61) | (hctx->total[1] << 3));
	put_be64(bits + 8, hctx->total[0] << 3);

	last = hctx->total[0] & (hctx->blk_size - 1);
	if (last < hctx->blk_size - len_size)
		padn = hctx->blk_size - len_size - last;
	else
		padn = 2 * hctx->blk_size - len_size - last;

	res = esp32s31_sha_update(ctx, pad, padn);
	if (res)
		return res;

	res = esp32s31_sha_update(ctx, bits + sizeof(bits) - len_size,
				  len_size);
	if (res)
		return res;

	memcpy(digest, hctx->state, MIN(len, hctx->dgt_size));

	return TEE_SUCCESS;
}

static void esp32s31_sha_free(struct crypto_hash_ctx *ctx)
{
	free(to_sha_ctx(ctx));
}

static void esp32s31_sha_copy_state(struct crypto_hash_ctx *dst_ctx,
				    struct crypto_hash_ctx *src_ctx)
{
	struct esp32s31_sha_ctx *dst = to_sha_ctx(dst_ctx);
	struct esp32s31_sha_ctx *src = to_sha_ctx(src_ctx);

	*dst = *src;
}

static const struct crypto_hash_ops esp32s31_sha_ops = {
	.init = esp32s31_sha_init,
	.update = esp32s31_sha_update,
	.final = esp32s31_sha_final,
	.free_ctx = esp32s31_sha_free,
	.copy_state = esp32s31_sha_copy_state,
};

static TEE_Result esp32s31_sha_alloc(struct crypto_hash_ctx **ctx,
				     uint32_t algo)
{
	struct esp32s31_sha_ctx *hctx = NULL;
	uint32_t mode = 0;
	size_t blk_size = 64;
	size_t state_size = 32;

	switch (algo) {
	case TEE_ALG_SHA1:
		mode = SHA_MODE_SHA1;
		state_size = TEE_SHA1_HASH_SIZE;
		break;
	case TEE_ALG_SHA224:
		mode = SHA_MODE_SHA224;
		break;
	case TEE_ALG_SHA256:
		mode = SHA_MODE_SHA256;
		break;
	case TEE_ALG_SHA384:
		mode = SHA_MODE_SHA384;
		blk_size = 128;
		state_size = 64;
		break;
	case TEE_ALG_SHA512:
		mode = SHA_MODE_SHA512;
		blk_size = 128;
		state_size = 64;
		break;
	default:
		return TEE_ERROR_NOT_IMPLEMENTED;
	}

	hctx = calloc(1, sizeof(*hctx));
	if (!hctx)
		return TEE_ERROR_OUT_OF_MEMORY;

	hctx->hash_ctx.ops = &esp32s31_sha_ops;
	hctx->mode = mode;
	hctx->blk_size = blk_size;
	hctx->state_size = state_size;
	hctx->dgt_size = TEE_ALG_GET_DIGEST_SIZE(algo);

	*ctx = &hctx->hash_ctx;

	return TEE_SUCCESS;
}

TEE_Result esp32s31_sha_register(void)
{
	sha.base.va = (vaddr_t)core_mmu_add_mapping(MEM_AREA_IO_SEC,
						    sha.base.pa, SHA_SIZE);
	if (!sha.base.va)
		panic("SHA mapping");

	esp32s31_crypto_enable(CRYPTO_SHA_CLK_EN, CRYPTO_SHA_RST_EN);

	return drvcrypt_register_hash(esp32s31_sha_alloc);
}
