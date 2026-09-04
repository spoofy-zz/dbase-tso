ifneq (,$(wildcard .env))
include .env
endif

ifeq ($(strip $(DBASE_TOOLCHAIN_BIN)),)
ifneq (,$(wildcard $(HOME)/.local/bin/cc370))
DBASE_TOOLCHAIN_BIN := $(HOME)/.local/bin
endif
endif

ifneq ($(strip $(DBASE_TOOLCHAIN_BIN)),)
export PATH := $(DBASE_TOOLCHAIN_BIN):$(PATH)
CC := $(DBASE_TOOLCHAIN_BIN)/cc370
AS := $(DBASE_TOOLCHAIN_BIN)/as370
LD := $(DBASE_TOOLCHAIN_BIN)/ld370
AR := $(DBASE_TOOLCHAIN_BIN)/ar370
endif

MBT_ROOT := mbt
include $(MBT_ROOT)/mk/mbt.mk

.PHONY: deploy-mvs deploy-mvs-dry-run

deploy-mvs:
	@tools/deploy-mvs.sh

deploy-mvs-dry-run:
	@tools/deploy-mvs.sh --dry-run
