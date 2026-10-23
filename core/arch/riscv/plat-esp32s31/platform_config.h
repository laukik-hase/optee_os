/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2026 Espressif Systems (Shanghai) Co. Ltd.
 *
 * Brief   ESP32-S31 platform configuration.
 */

#ifndef PLATFORM_CONFIG_H
#define PLATFORM_CONFIG_H

#include <mm/generic_ram_layout.h>
#include <riscv.h>

/* PSRAM: the whole 16 MiB is REE memory except the OP-TEE carve-out */
#ifndef DRAM_BASE
#define DRAM_BASE		0x50000000
#define DRAM_SIZE		0x01000000
#endif

#ifdef CFG_RISCV_MTIME_RATE
#define RISCV_MTIME_RATE CFG_RISCV_MTIME_RATE
#else
#define RISCV_MTIME_RATE 320000000
#endif

#define PLAT_THREAD_EXCP_FOREIGN_INTR	\
	(CSR_XIE_EIE | CSR_XIE_TIE | CSR_XIE_SIE)
#define PLAT_THREAD_EXCP_NATIVE_INTR	(0)

#endif /*PLATFORM_CONFIG_H*/
