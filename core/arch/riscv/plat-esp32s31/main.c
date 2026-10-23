// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2026 Espressif Systems (Shanghai) Co. Ltd.
 */

#include <kernel/boot.h>
#include <kernel/panic.h>
#include <mm/core_memprot.h>
#include <platform_config.h>

/* REE memory for dynamic shared memory; the carve-out is excluded */
register_ddr(DRAM_BASE, DRAM_SIZE);

/*
 * The CLIC routes no external interrupts to the trusted domain, so an
 * external interrupt reaching OP-TEE is a configuration error.
 */
void interrupt_main_handler(void)
{
	panic("Unexpected external interrupt");
}
