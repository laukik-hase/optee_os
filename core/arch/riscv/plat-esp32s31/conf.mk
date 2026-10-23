# ESP32-S31: RV32IMAC (dual-hart SoC, single hart used), Sv32 MMU, CLIC,
# 16 MiB PSRAM at 0x50000000 shared with the REE. OP-TEE owns a carve-out
# at the top of PSRAM and runs in an OpenSBI trusted domain; it is reached
# through the RPMI TEE / REQFWD service groups of the OpenSBI OP-TEE
# dispatcher (same model as plat-virt rv32).

$(call force,CFG_RV32_core,y)
$(call force,CFG_RISCV_MMU_MODE,32)
$(call force,CFG_RISCV_FPU,n)
$(call force,CFG_RISCV_ISA_C,y)
supported-ta-targets = ta_rv32

$(call force,CFG_CORE_LARGE_PHYS_ADDR,y)
$(call force,CFG_CORE_RESERVED_SHM,n)
$(call force,CFG_CORE_DYN_SHM,y)

# The dispatcher DT (cpus, rpmi-reqfwd/rpmi-tee nodes) arrives in a1
CFG_DT ?= y

# Crypto: no Zkr on this core, the hardware RNG peripheral comes later
$(call force,CFG_WITH_SOFTWARE_PRNG,y)
$(call force,CFG_RISCV_ZKR_RNG,n)

# Protection flags
$(call force,CFG_CORE_ASLR,n)
$(call force,CFG_CORE_SANITIZE_KADDRESS,n)
# Map core read-only data non-executable (W^X for .rodata, not just .data)
CFG_CORE_RODATA_NOEXEC ?= y

CFG_ENABLE_EMBEDDED_TESTS ?= y

# Hart-related flags: the REE (Linux) runs on hart 0 only today, so does
# OP-TEE. Two threads still let a second REE call enter while the first
# one waits in an RPC.
CFG_TEE_CORE_NB_CORE ?= 1
CFG_NUM_THREADS ?= 2
CFG_BOOT_SYNC_CPU ?= n

# Interrupt controller: CLIC, no PLIC/APLIC/IMSIC. OP-TEE takes no
# external interrupts in the trusted domain, so no driver is needed.
$(call force,CFG_RISCV_SMCLIC,y)

# FENCE.I does not write back the D-cache: maintain the caches through
# the OpenSBI ESP32-S31 vendor extension (see main.c)
$(call force,CFG_RISCV_PLAT_CACHE_OPS,y)
$(call force,CFG_RISCV_PLIC,n)
$(call force,CFG_RISCV_APLIC,n)
$(call force,CFG_RISCV_APLIC_MSI,n)
$(call force,CFG_RISCV_IMSIC,n)

# Console: SBI debug console (OpenSBI drives the ESP UART)
$(call force,CFG_RISCV_SBI_CONSOLE,y)
$(call force,CFG_16550_UART,n)

# SBI extensions
CFG_RISCV_SBI_MPXY ?= y
CFG_RISCV_SBI_MPXY_RPMI ?= y

$(call force,CFG_RISCV_M_MODE,n)
$(call force,CFG_RISCV_S_MODE,y)
$(call force,CFG_RISCV_TIME_SOURCE_RDTIME,y)
CFG_RISCV_MTIME_RATE ?= 320000000
CFG_RISCV_SBI ?= y
CFG_RISCV_WITH_M_MODE_SM ?= y

# Memory layout: top 3 MiB of the 16 MiB PSRAM. The core window is 1 MiB;
# what the core does not use (about half of it) is released to the TA pool
# at boot, so TAs get about 2.5 MiB, enough for xtest's largest TA (os_test,
# ~1 MB with its heap) next to a second TA. Must match the OpenSBI
# trusted-domain regions, the SPL FIT load address and the REE memory nodes.
CFG_TDDRAM_START ?= 0x50D00000
CFG_TDDRAM_SIZE  ?= 0x00300000
CFG_TEE_RAM_VA_SIZE ?= 0x00100000
