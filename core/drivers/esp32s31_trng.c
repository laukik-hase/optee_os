// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2026 Espressif Systems (Shanghai) Co. Ltd.
 *
 * ESP32-S31 true random number generator
 */

#include <crypto/crypto.h>
#include <initcall.h>
#include <io.h>
#include <kernel/delay.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>
#include <rng_support.h>
#include <string.h>
#include <trace.h>
#include <util.h>

/* TRNG registers */
#define TRNG_CONF			0x00
#define TRNG_DEBUG_CONF			0x04
#define TRNG_INT_RAW			0x2c
#define TRNG_INT_CLR			0x30
#define TRNG_CRC_SYNC_DATA		0x48
#define TRNG_DATE			0xfc
#define TRNG_SIZE			0x100

#define TRNG_CONF_NOISE_SRC_MASK	GENMASK_32(4, 0)
#define TRNG_CONF_NOISE_POS_MASK	GENMASK_32(9, 5)
#define TRNG_CONF_REPETITION_MASK	GENMASK_32(18, 10)
#define TRNG_CONF_ADAPTIVE_MASK		GENMASK_32(27, 19)
#define TRNG_CONF_OUTPUT_MODE		BIT(29)
#define TRNG_CONF_NOISE_CRC_EN		BIT(30)
#define TRNG_CONF_SAMPLE_EN		BIT(31)

#define TRNG_DEBUG_STARTUP_START	BIT(0)
#define TRNG_DEBUG_STARTUP_LIMIT_MASK	GENMASK_32(16, 1)
#define TRNG_DEBUG_HEALTH_ERR_MASK	GENMASK_32(28, 26)
#define TRNG_DEBUG_HEALTH_BYPASS	BIT(30)

#define TRNG_INT_ERROR			BIT(0)
#define TRNG_DATE_CLK_EN		BIT(28)

/* LP peripheral clock/reset control of the TRNG */
#define LP_PERICLKRST_RNG_CTRL		0x3c
#define LP_PERICLKRST_SIZE		0x100
#define RNG_CTRL_CLK_EN			BIT(30)
#define RNG_CTRL_RST_EN			BIT(31)

/* Noise source and thresholds validated for the ESP32-S31 TRNG */
#define TRNG_NOISE_SRC			BIT(4)
#define TRNG_NOISE_POS			BIT(4)
#define TRNG_REPETITION_CUTOFF		0x1f
#define TRNG_ADAPTIVE_CUTOFF		0x12
#define TRNG_STARTUP_LIMIT		1024

/* Minimum spacing between two reads of the data register */
#define TRNG_READ_DELAY_US		1

/* Restarts allowed before a health test failure is reported */
#define TRNG_MAX_RESTARTS		3

static struct esp32s31_trng {
	struct io_pa_va base;
	struct io_pa_va clkrst;
	unsigned int lock;
	bool ready;
} trng = {
	.base.pa = ESP32S31_TRNG_BASE,
	.clkrst.pa = ESP32S31_LP_PERICLKRST_BASE,
	.lock = SPINLOCK_UNLOCK,
};

static bool trng_health_error(void)
{
	return (io_read32(trng.base.va + TRNG_DEBUG_CONF) &
		TRNG_DEBUG_HEALTH_ERR_MASK) ||
	       (io_read32(trng.base.va + TRNG_INT_RAW) & TRNG_INT_ERROR);
}

static uint32_t trng_read_word(void)
{
	uint32_t val = io_read32(trng.base.va + TRNG_CRC_SYNC_DATA);

	udelay(TRNG_READ_DELAY_US);

	return val ^ io_read32(trng.base.va + TRNG_CRC_SYNC_DATA);
}

static void trng_hw_init(void)
{
	vaddr_t ctrl = trng.clkrst.va + LP_PERICLKRST_RNG_CTRL;

	io_setbits32(ctrl, RNG_CTRL_CLK_EN);
	io_setbits32(trng.base.va + TRNG_DATE, TRNG_DATE_CLK_EN);
	io_setbits32(ctrl, RNG_CTRL_RST_EN);
	io_clrbits32(ctrl, RNG_CTRL_RST_EN);

	io_clrsetbits32(trng.base.va + TRNG_CONF,
			TRNG_CONF_NOISE_SRC_MASK | TRNG_CONF_NOISE_POS_MASK |
			TRNG_CONF_REPETITION_MASK | TRNG_CONF_ADAPTIVE_MASK,
			SHIFT_U32(TRNG_NOISE_SRC, 0) |
			SHIFT_U32(TRNG_NOISE_POS, 5) |
			SHIFT_U32(TRNG_REPETITION_CUTOFF, 10) |
			SHIFT_U32(TRNG_ADAPTIVE_CUTOFF, 19));
	io_clrsetbits32(trng.base.va + TRNG_DEBUG_CONF,
			TRNG_DEBUG_STARTUP_LIMIT_MASK |
			TRNG_DEBUG_HEALTH_BYPASS,
			SHIFT_U32(TRNG_STARTUP_LIMIT, 1));
	io_write32(trng.base.va + TRNG_INT_CLR, TRNG_INT_ERROR);
	io_setbits32(trng.base.va + TRNG_CONF,
		     TRNG_CONF_OUTPUT_MODE | TRNG_CONF_NOISE_CRC_EN |
		     TRNG_CONF_SAMPLE_EN);
	io_setbits32(trng.base.va + TRNG_DEBUG_CONF, TRNG_DEBUG_STARTUP_START);
}

TEE_Result hw_get_random_bytes(void *buf, size_t len)
{
	unsigned int restarts = 0;
	uint8_t *out = buf;
	uint32_t exceptions = 0;
	uint32_t val = 0;
	size_t n = 0;

	if (!trng.ready)
		return TEE_ERROR_BAD_STATE;

	exceptions = cpu_spin_lock_xsave(&trng.lock);

	while (len) {
		val = trng_read_word();

		/* Health errors latch: drop the word and restart the TRNG */
		if (trng_health_error()) {
			DMSG("TRNG health error %#"PRIx32,
			     io_read32(trng.base.va + TRNG_DEBUG_CONF));
			if (restarts++ == TRNG_MAX_RESTARTS) {
				cpu_spin_unlock_xrestore(&trng.lock,
							 exceptions);
				EMSG("TRNG health test failure");
				return TEE_ERROR_BAD_STATE;
			}
			trng_hw_init();
			continue;
		}

		n = MIN(len, sizeof(val));
		memcpy(out, &val, n);
		out += n;
		len -= n;
	}

	cpu_spin_unlock_xrestore(&trng.lock, exceptions);

	return TEE_SUCCESS;
}

static TEE_Result esp32s31_trng_init(void)
{
	trng.base.va = (vaddr_t)core_mmu_add_mapping(MEM_AREA_IO_SEC,
						     trng.base.pa, TRNG_SIZE);
	trng.clkrst.va = (vaddr_t)core_mmu_add_mapping(MEM_AREA_IO_SEC,
						       trng.clkrst.pa,
						       LP_PERICLKRST_SIZE);
	if (!trng.base.va || !trng.clkrst.va)
		panic("TRNG mapping");

	trng_hw_init();
	trng.ready = true;

	return TEE_SUCCESS;
}

early_init(esp32s31_trng_init);
