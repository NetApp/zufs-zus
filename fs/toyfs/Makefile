# SPDX-License-Identifier: BSD-3-Clause
#
# Makefile for the toyfs file-system via zufs
#
# Copyright (C) 2018 NetApp, Inc. All rights reserved.
#
# See module.c for LICENSE details.
#
# Authors:
#	Shachar Sharon <sshachar@netapp.com>
#
TOYFS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ZDIR?=$(TOYFS_DIR)../..
TOYMKFS := $(TOYFS_DIR)mkfs.toyfs
TOYMKFS_CLEAN := toymkfs_clean
TOYMKFS_OBJS := $(TOYFS_DIR)objs/mkfs.o
TOYMKFS_FLAGS := -I $(ZDIR) -L$(ZDIR) -luuid -lzus

ZM_NAME := toyfs
ZM_OBJS := common.o super.o inode.o dir.o namei.o symlink.o file.o xattr.o mmap.o
ZM_LIBS := uuid
ZM_PRE_BUILD := mkfs.toyfs
ZM_PRE_CLEAN := mkfs.toyfs_clean

all:
	@$(MAKE) M=$(PWD) -C $(ZDIR) module

clean:
	@$(MAKE) M=$(PWD) -C $(ZDIR) module_clean

mkfs.toyfs:
	$(CC) $(TOYMKFS_FLAGS) -o $@ $(TOYFS_DIR)mkfs.c

mkfs.toyfs_clean:
	rm -vf $(TOYMKFS_OBJS) $(TOYMKFS)
