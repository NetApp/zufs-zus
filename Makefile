# SPDX-License-Identifier: BSD-3-Clause
#
# Makefile for the zus user-mode application
#
# Copyright (C) 2018 NetApp, Inc.  All rights reserved.
#
# See module.c for LICENSE details.
#
# Authors:
#	Omer Caspi <omerc@netapp.com>
ifeq ($(M),)
-include .config
MAKEFLAGS := --no-print-directory
LIBFS_DIRS := $(addprefix fs/,$(CONFIG_LIBFS_MODULES))
LIBFS_CLEAN := $(addprefix clean_,$(CONFIG_LIBFS_MODULES))
core:
	@echo "Building ZUS library"
	@$(MAKE) -f zuslib.mk
	@echo "Building ZUS daemon"
	@$(MAKE) -f zusd.mk

clean: $(LIBFS_CLEAN)
	@echo "Cleaning ZUS core"
	@$(MAKE) -f zuslib.mk __clean
	@$(MAKE) -f zusd.mk __clean

$(CONFIG_LIBFS_MODULES): core
	@echo "Building $@ ZUS FS module"
	@$(MAKE) M=fs/$@ -C $(CURDIR) module

clean_%:
	$(eval NAME := $(patsubst clean_%,%,$(@)))
	@echo "Cleaning $(NAME) ZUS FS module"
	@$(MAKE) M=fs/$(NAME) -C $(CURDIR) module_clean

all: core $(CONFIG_LIBFS_MODULES)

install:
	pkg/install.sh
rpm:
	pkg/create_pkg.sh
cscope:
	find . -type f -name '*.[c|h]' > cscope.files
	find . -type l -name '*.[c|h]' -exec realpath \
			--relative-to=$(CURDIR) '{}' \; >> cscope.files
	cscope -bcqR

.PHONY: install rpm all clean cscope
.NOTPARALLEL:
.DEFAULT_GOAL := all
else
include zusfs.mk
endif
