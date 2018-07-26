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

fs/toyfs/libtoyfs.a: $(toyfs_OBJ)
	ar rcs $(LDFLAGS) -o $@ $^

fs_libs+=fs/toyfs/libtoyfs.a


toyfs_mkfs_OBJ = fs/toyfs/mkfs.o md.o
toyfs_mkfs_PROG = fs/toyfs/mkfs.toyfs

ALL += $(toyfs_mkfs_PROG)

all: $(toyfs_mkfs_PROG)

$(toyfs_mkfs_PROG):  $(toyfs_mkfs_OBJ)
	$(CC) $(LDFLAGS) $(CFLAGS) $(C_LIBS) -o $@ $^


