#
## Makefile for the STMicroelectronics touchscreen driver.
#

obj-$(CONFIG_TOUCHSCREEN_ST_I2C) += fts.o fts_gui.o fts_driver_test.o fts_lib/

subdir-ccflags-y += \
	-Wno-typedef-redefinition \
	-Wno-constant-logical-operand
