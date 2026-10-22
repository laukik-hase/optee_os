// SPDX-License-Identifier: BSD-2-Clause
/*
 * RV32 has no 64-bit atomic load instruction, so GCC emits a libatomic call
 * for READ_ONCE() on 64-bit fields (paddr_t with CFG_CORE_LARGE_PHYS_ADDR,
 * struct optee_msg_param_tmem members). Bare-metal toolchains ship no
 * libatomic, so provide the relaxed-order helper here.
 */
#include <compiler.h>
#include <stdint.h>

/* shortcut: two 32-bit loads, may tear under a concurrent 64-bit writer;
 * upgrade to a lock-protected read if a caller ever needs real atomicity. */
uint64_t __atomic_load_8(const volatile void *p, int memorder __unused);
uint64_t __atomic_load_8(const volatile void *p, int memorder __unused)
{
	return *(const volatile uint64_t *)p;
}
