# SPDX-License-Identifier: BSD-3-Clause
#
# Makefile for foofs
#
# Copyright (C) 2018 NetApp, Inc. All rights reserved.
#
# See module.c for LICENSE details.
#
# Authors:
#	Omer Caspi <omerc@netapp.com>

FOOFS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ZDIR?=$(FOOFS_DIR)../..
ZM_NAME := foofs
ZM_OBJS := foofs.o

all:
	$(MAKE) M=$(PWD) -C $(ZDIR) module
clean:
	$(MAKE) M=$(PWD) -C $(ZDIR) module_clean
