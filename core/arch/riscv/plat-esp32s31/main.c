// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright 2026 Espressif Systems (Shanghai) Co. Ltd.
 */

#include <kernel/boot.h>
#include <kernel/panic.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>
#include <riscv.h>
#include <sbi.h>
#include <trace.h>
#include <util.h>

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

/*
 * Cache maintenance. The ESP32-S31 L1 caches are managed by ROM routines
 * that only run in M-mode; FENCE.I alone does not write dirty D-cache
 * lines back, so code written by the core (ldelf, TAs) would be fetched
 * stale. OpenSBI exposes the ROM routines as a vendor SBI extension, the
 * same one the Linux driver drivers/cache/esp32s31_cache.c uses.
 */
#define SBI_EXT_VENDOR_START		0x09000000UL
#define SBI_EXT_VENDOR_END		0x09FFFFFFUL

#define S31_SBI_CACHE_WBACK		0
#define S31_SBI_CACHE_INVAL		1
#define S31_SBI_CACHE_WBACK_INVAL	2
#define S31_SBI_ICACHE_SYNC		3
#define S31_SBI_ICACHE_SYNC_RANGE	4
#define S31_SBI_DCACHE_WBACK_ALL	5

static unsigned long s31_cache_extid(void)
{
	static unsigned long extid;

	if (!extid) {
		struct sbiret ret = sbi_ecall(SBI_EXT_BASE,
					      SBI_EXT_BASE_GET_MVENDORID);

		if (ret.error)
			panic("SBI mvendorid");
		extid = SBI_EXT_VENDOR_START +
			(ret.value & (SBI_EXT_VENDOR_END -
				      SBI_EXT_VENDOR_START));
	}

	return extid;
}

static long s31_cache_try(unsigned long fid, paddr_t pa, size_t len)
{
	/*
	 * Resolve the extension ID before sbi_ecall(): its arguments are
	 * bound to a0-a7 register variables, so a function call among them
	 * would clobber the registers already loaded.
	 */
	unsigned long extid = s31_cache_extid();
	struct sbiret ret = sbi_ecall(extid, fid, pa, len);

	if (ret.error)
		EMSG("cache SBI ext %#lx fid %lu pa %#"PRIxPA" len %#zx: error %ld",
		     extid, fid, pa, len, ret.error);
	return ret.error;
}

static void s31_cache_call(unsigned long fid, paddr_t pa, size_t len)
{
	if (s31_cache_try(fid, pa, len))
		panic("ESP32-S31 cache SBI call failed");
}

static void s31_icache_sync_all(void)
{
	s31_cache_call(S31_SBI_DCACHE_WBACK_ALL, 0, 0);
	s31_cache_call(S31_SBI_ICACHE_SYNC, 0, 0);
	asm volatile ("fence.i" ::: "memory");
}

/*
 * Apply @fid to [va, va + len) one page at a time, since the range may be
 * virtually contiguous only. Returns false when a page has no physical
 * address in the current mappings.
 */
static bool s31_cache_range(unsigned long fid, void *va, size_t len)
{
	vaddr_t v = (vaddr_t)va;
	vaddr_t end = v + len;

	while (v < end) {
		vaddr_t next = MIN(ROUNDUP(v + 1, SMALL_PAGE_SIZE), end);
		paddr_t pa = virt_to_phys((void *)v);

		/* Unmapped or rejected range: caller uses whole-cache ops */
		if (!pa || s31_cache_try(fid, pa, next - v))
			return false;
		v = next;
	}

	return true;
}

TEE_Result plat_cache_op_inner(enum cache_op op, void *va, size_t len)
{
	mb();

	switch (op) {
	case DCACHE_CLEAN:
	case DCACHE_CLEAN_INV:
		s31_cache_call(S31_SBI_DCACHE_WBACK_ALL, 0, 0);
		break;
	case DCACHE_INVALIDATE:
		/* No whole-cache invalidate; single hart, caches coherent */
		break;
	case DCACHE_AREA_CLEAN:
		if (!s31_cache_range(S31_SBI_CACHE_WBACK, va, len))
			s31_cache_call(S31_SBI_DCACHE_WBACK_ALL, 0, 0);
		break;
	case DCACHE_AREA_INVALIDATE:
		if (!s31_cache_range(S31_SBI_CACHE_INVAL, va, len))
			panic("ESP32-S31 D-cache range invalidate");
		break;
	case DCACHE_AREA_CLEAN_INV:
		if (!s31_cache_range(S31_SBI_CACHE_WBACK_INVAL, va, len))
			s31_cache_call(S31_SBI_DCACHE_WBACK_ALL, 0, 0);
		break;
	case ICACHE_INVALIDATE:
		s31_icache_sync_all();
		break;
	case ICACHE_AREA_INVALIDATE:
		if (s31_cache_range(S31_SBI_CACHE_WBACK, va, len) &&
		    s31_cache_range(S31_SBI_ICACHE_SYNC_RANGE, va, len))
			asm volatile ("fence.i" ::: "memory");
		else
			s31_icache_sync_all();
		break;
	default:
		return TEE_ERROR_NOT_IMPLEMENTED;
	}

	return TEE_SUCCESS;
}
