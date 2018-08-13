#
# Makefile for the zus POC user-mode application
#
# Copyright (C) 2018 NetApp, Inc.  All rights reserved.
#
# ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
#
# Authors:
#	Boaz Harrosh <boazh@netapp.com>
#	Shachar Sharon <sshachar@netapp.com>
#

LIBFS_SUBDIRS = $(addprefix fs/,$(CONFIG_LIBFS_MODULES))

all clean: $(LIBFS_SUBDIRS)

$(LIBFS_SUBDIRS): $(LIBZUS)
	$(MAKE) -C $@ $(MAKECMDGOALS)

.PHONY: $(LIBFS_SUBDIRS)
