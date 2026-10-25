/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2026 Espressif Systems (Shanghai) Co. Ltd.
 */

#ifndef __ESP32S31_CRYPTO_H__
#define __ESP32S31_CRYPTO_H__

#include <stdint.h>
#include <tee_api_types.h>
#include <util.h>

/* Per-engine clock enable and reset bits in HP_SYS_CLKRST CRYPTO_CTRL0 */
#define CRYPTO_AES_CLK_EN		BIT(4)
#define CRYPTO_AES_RST_EN		BIT(5)
#define CRYPTO_SHA_CLK_EN		BIT(6)
#define CRYPTO_SHA_RST_EN		BIT(7)

/* Enable the clock of one engine and pulse its reset */
void esp32s31_crypto_enable(uint32_t clk_en, uint32_t rst_en);

TEE_Result esp32s31_sha_register(void);
TEE_Result esp32s31_aes_register(void);

#endif /* __ESP32S31_CRYPTO_H__ */
