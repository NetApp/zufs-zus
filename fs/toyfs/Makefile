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

toyfs_OBJ = common.o super.o inode.o dir.o namei.o symlink.o file.o

toyfs_mkfs = mkfs.toyfs
toyfs_lib = libtoyfs.so

all: $(toyfs_mkfs) $(toyfs_lib)

clean:
	@rm -vf *.o $(toyfs_lib) $(toyfs_mkfs)

CFLAGS += -I../..
LDFLAGS += -L../.. -luuid -lzus -lpmem

$(toyfs_mkfs): mkfs.o
	$(CC) $(LDFLAGS) $(CFLAGS) $(C_LIBS) -o $@ $^

$(toyfs_lib): $(toyfs_OBJ)
	$(CC) -shared $(LDFLAGS) $(CFLAGS) $(C_LIBS) -o $@ $^
