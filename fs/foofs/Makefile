#
# Makefile for foofs
#
# Copyright (C) 2018 NetApp, Inc. All rights reserved.
#
# ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
#
# Authors:
#	Boaz Harrosh <boaz@plexistor.com>
#	Shachar Sharon <sshachar@netapp.com>
#

foofs_OBJ = foofs.o

foofs_lib = libfoofs.so

all: $(foofs_lib)

clean:
	@rm -vf *.o $(foofs_lib)

CFLAGS += -Wno-missing-prototypes -Wno-missing-declarations
CFLAGS += -I../..

$(foofs_lib): $(foofs_OBJ)
	$(CC) -shared $(LDFLAGS) $(CFLAGS) $(C_LIBS) -o $@ $^
