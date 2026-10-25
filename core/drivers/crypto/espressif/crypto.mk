# CFG_ESP32S31_CRYPTO_DRIVER, when enabled, embeds
#       ESP32-S31 HW cryptographic support and OP-TEE Crypto Driver.
# CFG_ESP32S31_SHA, when enabled, embeds
#       ESP32-S31 SHA accelerator support,
#       HASH Crypto Driver.

ifeq ($(CFG_ESP32S31_CRYPTO_DRIVER),y)

$(call force,CFG_CRYPTO_DRIVER,y)
CFG_CRYPTO_DRIVER_DEBUG ?= 0

ifeq ($(CFG_ESP32S31_SHA),y)
$(call force,CFG_CRYPTO_DRV_HASH,y,Mandated by CFG_ESP32S31_SHA)
endif

endif # CFG_ESP32S31_CRYPTO_DRIVER
