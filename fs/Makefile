#
# Makefile for the zus POC user-mode application
#
# Copyright (C) 2018 NetApp, Inc.  All rights reserved.
#
# ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
#
# Authors:
#	Boaz Harrosh <boaz@plexistor.com>
#	Shachar Sharon <sshachar@netapp.com>
#

# foofs linked into zus
fs/libfoofs.a: fs/foofs.o
	ar rcs $(LDFLAGS) -o $@ $^

fs_libs+=fs/libfoofs.a
