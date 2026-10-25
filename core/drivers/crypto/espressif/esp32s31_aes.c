// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2026 Espressif Systems (Shanghai) Co. Ltd.
 *
 * ESP32-S31 AES accelerator
 */

#include <assert.h>
#include <drvcrypt.h>
#include <drvcrypt_cipher.h>
#include <io.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <malloc.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>
#include <string.h>
#include <string_ext.h>
#include <tee_api_types.h>
#include <trace.h>
#include <utee_defines.h>
#include <util.h>

#include "esp32s31_crypto.h"

/* AES registers */
#define AES_KEY				0x00
#define AES_TEXT_IN			0x20
#define AES_TEXT_OUT			0x30
#define AES_MODE			0x40
#define AES_TRIGGER			0x48
#define AES_STATE			0x4c
#define AES_SIZE			0x100

#define AES_MODE_DECRYPT		BIT(2)
#define AES_STATE_IDLE			0

#define AES_MAX_KEY_SIZE		32
#define AES_TIMEOUT_US			1000

struct esp32s31_aes_ctx {
	uint32_t algo;
	bool decrypt;
	size_t key_len;
	uint8_t key[AES_MAX_KEY_SIZE];
	/* CBC chaining value or CTR counter block */
	uint8_t iv[TEE_AES_BLOCK_SIZE];
	/* CTR keystream and how much of it is already used */
	uint8_t ks[TEE_AES_BLOCK_SIZE];
	size_t ks_off;
};

static struct esp32s31_aes {
	struct io_pa_va base;
	struct mutex lock;
} aes = {
	.base.pa = ESP32S31_AES_BASE,
	.lock = MUTEX_INITIALIZER,
};

static void aes_load_key(struct esp32s31_aes_ctx *actx, bool decrypt)
{
	uint32_t word = 0;
	size_t i = 0;

	io_write32(aes.base.va + AES_MODE, (actx->key_len / 8 - 2) |
		   (decrypt ? AES_MODE_DECRYPT : 0));

	for (i = 0; i < actx->key_len; i += sizeof(word)) {
		memcpy(&word, actx->key + i, sizeof(word));
		io_write32(aes.base.va + AES_KEY + i, word);
	}
}

static void aes_clear_key(void)
{
	size_t i = 0;

	for (i = 0; i < AES_MAX_KEY_SIZE; i += sizeof(uint32_t))
		io_write32(aes.base.va + AES_KEY + i, 0);
}

/* Run one block through the engine with the loaded key and mode */
static TEE_Result aes_block(const uint8_t *in, uint8_t *out)
{
	uint32_t state = 0;
	uint32_t word = 0;
	size_t i = 0;

	for (i = 0; i < TEE_AES_BLOCK_SIZE; i += sizeof(word)) {
		memcpy(&word, in + i, sizeof(word));
		io_write32(aes.base.va + AES_TEXT_IN + i, word);
	}

	io_write32(aes.base.va + AES_TRIGGER, 1);

	if (IO_READ32_POLL_TIMEOUT(aes.base.va + AES_STATE, state,
				   state == AES_STATE_IDLE, 0, AES_TIMEOUT_US))
		return TEE_ERROR_BUSY;

	for (i = 0; i < TEE_AES_BLOCK_SIZE; i += sizeof(word)) {
		word = io_read32(aes.base.va + AES_TEXT_OUT + i);
		memcpy(out + i, &word, sizeof(word));
	}

	return TEE_SUCCESS;
}

static void ctr_inc(uint8_t *ctr)
{
	int i = 0;

	for (i = TEE_AES_BLOCK_SIZE - 1; i >= 0; i--)
		if (++ctr[i])
			break;
}

static TEE_Result aes_ecb(const uint8_t *src, uint8_t *dst, size_t len)
{
	TEE_Result res = TEE_SUCCESS;

	for (; len; len -= TEE_AES_BLOCK_SIZE) {
		res = aes_block(src, dst);
		if (res)
			return res;
		src += TEE_AES_BLOCK_SIZE;
		dst += TEE_AES_BLOCK_SIZE;
	}

	return TEE_SUCCESS;
}

static TEE_Result aes_cbc(struct esp32s31_aes_ctx *actx, const uint8_t *src,
			  uint8_t *dst, size_t len)
{
	uint8_t blk[TEE_AES_BLOCK_SIZE] = { };
	TEE_Result res = TEE_SUCCESS;
	size_t i = 0;

	for (; len; len -= TEE_AES_BLOCK_SIZE) {
		if (actx->decrypt) {
			/* src and dst may overlap: keep the ciphertext */
			memcpy(blk, src, sizeof(blk));
			res = aes_block(blk, dst);
			if (res)
				break;
			for (i = 0; i < TEE_AES_BLOCK_SIZE; i++)
				dst[i] ^= actx->iv[i];
			memcpy(actx->iv, blk, sizeof(blk));
		} else {
			for (i = 0; i < TEE_AES_BLOCK_SIZE; i++)
				blk[i] = src[i] ^ actx->iv[i];
			res = aes_block(blk, dst);
			if (res)
				break;
			memcpy(actx->iv, dst, sizeof(blk));
		}
		src += TEE_AES_BLOCK_SIZE;
		dst += TEE_AES_BLOCK_SIZE;
	}

	memzero_explicit(blk, sizeof(blk));

	return res;
}

static TEE_Result aes_ctr(struct esp32s31_aes_ctx *actx, const uint8_t *src,
			  uint8_t *dst, size_t len)
{
	TEE_Result res = TEE_SUCCESS;
	size_t i = 0;

	for (i = 0; i < len; i++) {
		if (actx->ks_off == TEE_AES_BLOCK_SIZE) {
			res = aes_block(actx->iv, actx->ks);
			if (res)
				return res;
			ctr_inc(actx->iv);
			actx->ks_off = 0;
		}
		dst[i] = src[i] ^ actx->ks[actx->ks_off++];
	}

	return TEE_SUCCESS;
}

static TEE_Result esp32s31_aes_init(struct drvcrypt_cipher_init *dinit)
{
	struct esp32s31_aes_ctx *actx = dinit->ctx;
	size_t iv_len = actx->algo == TEE_ALG_AES_ECB_NOPAD ?
			0 : TEE_AES_BLOCK_SIZE;

	if (dinit->key1.length != 16 && dinit->key1.length != 24 &&
	    dinit->key1.length != 32)
		return TEE_ERROR_BAD_PARAMETERS;
	if (dinit->iv.length != iv_len)
		return TEE_ERROR_BAD_PARAMETERS;

	actx->decrypt = !dinit->encrypt;
	actx->key_len = dinit->key1.length;
	memcpy(actx->key, dinit->key1.data, actx->key_len);
	if (iv_len)
		memcpy(actx->iv, dinit->iv.data, iv_len);
	actx->ks_off = TEE_AES_BLOCK_SIZE;

	return TEE_SUCCESS;
}

static TEE_Result esp32s31_aes_update(struct drvcrypt_cipher_update *dupdate)
{
	struct esp32s31_aes_ctx *actx = dupdate->ctx;
	const uint8_t *src = dupdate->src.data;
	uint8_t *dst = dupdate->dst.data;
	size_t len = dupdate->src.length;
	TEE_Result res = TEE_SUCCESS;
	bool ctr = actx->algo == TEE_ALG_AES_CTR;

	if (dupdate->dst.length < len)
		return TEE_ERROR_SHORT_BUFFER;
	if (!ctr && len % TEE_AES_BLOCK_SIZE)
		return TEE_ERROR_BAD_PARAMETERS;
	if (!len)
		return TEE_SUCCESS;

	mutex_lock(&aes.lock);

	/* CTR only ever encrypts the counter block */
	aes_load_key(actx, actx->decrypt && !ctr);

	if (ctr)
		res = aes_ctr(actx, src, dst, len);
	else if (actx->algo == TEE_ALG_AES_CBC_NOPAD)
		res = aes_cbc(actx, src, dst, len);
	else
		res = aes_ecb(src, dst, len);

	aes_clear_key();

	mutex_unlock(&aes.lock);

	return res;
}

static void esp32s31_aes_final(void *ctx __unused)
{
}

static void esp32s31_aes_free(void *ctx)
{
	struct esp32s31_aes_ctx *actx = ctx;

	if (actx)
		memzero_explicit(actx, sizeof(*actx));
	free(actx);
}

static void esp32s31_aes_copy_state(void *dst_ctx, void *src_ctx)
{
	memcpy(dst_ctx, src_ctx, sizeof(struct esp32s31_aes_ctx));
}

static TEE_Result esp32s31_aes_alloc(void **ctx, uint32_t algo)
{
	struct esp32s31_aes_ctx *actx = NULL;

	switch (algo) {
	case TEE_ALG_AES_ECB_NOPAD:
	case TEE_ALG_AES_CBC_NOPAD:
	case TEE_ALG_AES_CTR:
		break;
	default:
		return TEE_ERROR_NOT_IMPLEMENTED;
	}

	actx = calloc(1, sizeof(*actx));
	if (!actx)
		return TEE_ERROR_OUT_OF_MEMORY;

	actx->algo = algo;
	*ctx = actx;

	return TEE_SUCCESS;
}

static struct drvcrypt_cipher esp32s31_aes_ops = {
	.alloc_ctx = esp32s31_aes_alloc,
	.free_ctx = esp32s31_aes_free,
	.init = esp32s31_aes_init,
	.update = esp32s31_aes_update,
	.final = esp32s31_aes_final,
	.copy_state = esp32s31_aes_copy_state,
};

TEE_Result esp32s31_aes_register(void)
{
	aes.base.va = (vaddr_t)core_mmu_add_mapping(MEM_AREA_IO_SEC,
						    aes.base.pa, AES_SIZE);
	if (!aes.base.va)
		panic("AES mapping");

	esp32s31_crypto_enable(CRYPTO_AES_CLK_EN, CRYPTO_AES_RST_EN);

	return drvcrypt_register_cipher(&esp32s31_aes_ops);
}
