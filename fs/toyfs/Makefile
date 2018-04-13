#
# Makefile for the toyfs file-system via zufs
#
# Copyright (C) 2018 NetApp, Inc. All rights reserved.
#
# ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
#
# Authors:
#	Shachar Sharon <sshachar@netapp.com>
#

toyfs_OBJ = $(addprefix fs/toyfs/, \
              common.o super.o inode.o dir.o namei.o symlink.o file.o)

fs_builtin += $(toyfs_OBJ)


