#
# Makefile for the zus POC user-mode application
#
# Copyright (C) 2018 NetApp, Inc.  All rights reserved.
#
# See module.c for LICENSE details.
#
# Authors:
#	Boaz Harrosh <boazh@netapp.com>
#	Shachar Sharon <sshachar@netapp.com>
#

all-libs	= $(addprefix all-,$(CONFIG_LIBFS_MODULES))
clean-libs	= $(addprefix clean-,$(CONFIG_LIBFS_MODULES))

all: $(all-libs)
clean: $(clean-libs)

$(all-libs): all-% : fs/%
	$(MAKE) -C $< all

$(all-libs): $(LIBZUS)

$(clean-libs): clean-% : fs/%
	$(MAKE) -C $< clean

.PHONY: $(all-libs) $(clean-libs)
