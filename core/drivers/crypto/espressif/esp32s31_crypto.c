// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2026 Espressif Systems (Shanghai) Co. Ltd.
 *
 * ESP32-S31 crypto accelerators: shared clocks and engine registration
 */

#include <config.h>
#include <initcall.h>
#include <io.h>
#include <kernel/panic.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>
#include <trace.h>

#include "esp32s31_crypto.h"

/* HP_SYS_CLKRST crypto clock/reset control */
#define CLKRST_CRYPTO_CTRL0		0x58
#define CLKRST_REF_240M_CTRL0		0x17c
#define CLKRST_SIZE			0x200

#define CRYPTO_SYS_CLK_EN		BIT(0)
#define CRYPTO_RST_EN			BIT(1)
#define CRYPTO_SEC_CLK_EN		BIT(2)
#define CRYPTO_DS_RST_EN		BIT(11)
#define CRYPTO_HMAC_RST_EN		BIT(15)
#define CRYPTO_ECDSA_RST_EN		BIT(17)
#define CRYPTO_CLK_SRC_SEL_MASK		GENMASK_32(21, 20)
#define CRYPTO_CLK_SRC_PLL_F240M	2
#define REF_240M_CLK_EN			BIT(8)

static struct io_pa_va clkrst = { .pa = ESP32S31_HP_SYS_CLKRST_BASE };

void esp32s31_crypto_enable(uint32_t clk_en, uint32_t rst_en)
{
	vaddr_t ctrl = clkrst.va + CLKRST_CRYPTO_CTRL0;

	io_setbits32(ctrl, clk_en);
	io_setbits32(ctrl, rst_en);
	io_clrbits32(ctrl, rst_en);
}

static void crypto_clk_init(void)
{
	vaddr_t ctrl = clkrst.va + CLKRST_CRYPTO_CTRL0;

	/* Crypto clock from PLL_F240M instead of the XTAL reset default */
	io_setbits32(clkrst.va + CLKRST_REF_240M_CTRL0, REF_240M_CLK_EN);
	io_setbits32(ctrl, CRYPTO_SYS_CLK_EN | CRYPTO_SEC_CLK_EN);
	io_clrsetbits32(ctrl, CRYPTO_CLK_SRC_SEL_MASK,
			SHIFT_U32(CRYPTO_CLK_SRC_PLL_F240M, 20));

	/* Engines stay in reset while the parent or a dependent block is */
	io_clrbits32(ctrl, CRYPTO_RST_EN | CRYPTO_DS_RST_EN |
		     CRYPTO_HMAC_RST_EN | CRYPTO_ECDSA_RST_EN);
}

static TEE_Result esp32s31_crypto_init(void)
{
	TEE_Result res = TEE_SUCCESS;

	clkrst.va = (vaddr_t)core_mmu_add_mapping(MEM_AREA_IO_SEC, clkrst.pa,
						  CLKRST_SIZE);
	if (!clkrst.va)
		panic("crypto clock mapping");

	crypto_clk_init();

	if (IS_ENABLED(CFG_ESP32S31_SHA)) {
		res = esp32s31_sha_register();
		if (res) {
			EMSG("SHA registration failed: %#"PRIx32, res);
			return res;
		}
	}

	if (IS_ENABLED(CFG_ESP32S31_AES)) {
		res = esp32s31_aes_register();
		if (res) {
			EMSG("AES registration failed: %#"PRIx32, res);
			return res;
		}
	}

	return TEE_SUCCESS;
}

driver_init(esp32s31_crypto_init);
