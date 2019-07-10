# SPDX-License-Identifier: BSD-3-Clause
#
# Makefile for the zus
#
# Copyright (C) 2018 NetApp, Inc.  All rights reserved.
#
# See module.c for LICENSE details.
#
# Authors:
#	Omer Caspi <omerc@netapp.com>
#
-include $(CURDIR)/.config
PROJ_NAME := zus
PROJ_TARGET_TYPE := lib
PROJ_OBJS := zus-core.o zus-vfs.o module.o md_zus.o nvml_movnt.o utils.o fs-loader.o pa.o
PROJ_OBJS += printz.o slab.o
PROJ_INCLUDES := .
PROJ_LIBS := rt uuid unwind dl pthread systemd

ifeq ($(CONFIG_TRY_ANON_MMAP),1)
PROJ_CDEFS += CONFIG_TRY_ANON_MMAP=1
else
PROJ_CDEFS += CONFIG_TRY_ANON_MMAP=0
endif

ifdef CONFIG_ZUF_DEF_PATH
PROJ_CDEFS += CONFIG_ZUF_DEF_PATH=\"$(CONFIG_ZUF_DEF_PATH)\"
endif

ZUS_API_H := zus_api.h md_def.h md.h
LINUX_STAT_H := linux/stat.h
LINKED_HEADERS := $(ZUS_API_H) $(LINUX_STAT_H)

$(ZUS_API_H):
	@ln -sTfv $(shell realpath --relative-to=$(ZDIR) \
		  $(ZUF_KERN_DIR))/fs/zuf/$@ $@

$(LINUX_STAT_H):
	@mkdir -p linux/ ;					\
	ln -sTfv $(shell realpath --relative-to=$(ZDIR)/linux \
		  $(ZUF_KERN_DIR))/include/uapi/$(LINUX_STAT_H) $(LINUX_STAT_H)

PROJ_OBJS_DEPS := zuslib.mk $(LINKED_HEADERS)

clean_headers:
	rm -f $(LINKED_HEADERS)

PROJ_CLEAN_DEPS := clean_headers

include common.zus.mk
