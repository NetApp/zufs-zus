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
#
-include $(CURDIR)/.config
PROJ_NAME := zusd
PROJ_OBJS := main.o signals.o
PROJ_INCLUDES += .
PROJ_LIBS := zus
PROJ_LIB_DIRS := .
PROJ_OBJS_DEPS := zusd.mk
PROJ_TARGET_DEPS := libzus.so

include common.zus.mk
