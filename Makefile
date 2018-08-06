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

# Globals
ROOT := $(dir $(lastword $(MAKEFILE_LIST)))
DEPEND := $(ROOT)/.dependencies
CONFIG := $(ROOT)/.config

# -Wframe-larger-than=$(FRAME_SIZE)
FRAME_SIZE = 4096
# -Wlarger-than=$(LARGER)
LARGER = 4096

-include $(CONFIG)

# list of -I -i dir(s)
C_INCLUDE = -I./ $(CONFIG_C_INCLUDE)

# List of -D defines
C_DEFINE = $(CONFIG_C_DEFINE)

ifdef CONFIG_ZUF_DEF_PATH
C_DEFINE += -DCONFIG_ZUF_DEF_PATH=\"$(CONFIG_ZUF_DEF_PATH)\"
endif

# What to warn about
CWARN =  -W -Werror -Wall
CWARN += -Wwrite-strings -Wmissing-prototypes  -Wundef -Wcast-qual
CWARN += -Wmissing-declarations -Wnested-externs -Wstrict-prototypes
CWARN += -Wbad-function-cast -Wcast-align -Wold-style-definition -Wextra
CWARN += -Wunused -Wshadow -Wfloat-equal -Wcomment -Wsign-compare -Waddress
CWARN += -Wredundant-decls -Wmissing-include-dirs -Wunknown-pragmas
CWARN += -Wparentheses -Wsequence-point -Wunused-macros -Wendif-labels
CWARN += -Woverlength-strings -Wunreachable-code -Wmissing-field-initializers
CWARN += -Waggregate-return -Wdeclaration-after-statement -Winit-self
CWARN += -Wswitch-default -Wswitch -Wswitch-enum
CWARN += -Wframe-larger-than=$(FRAME_SIZE) -Wlarger-than=$(LARGER)
CWARN += $(CONFIG_CWARN)

CNO_WARN := 	-Wno-unused-parameter -Wno-missing-field-initializers \
		$(CONFIG_CNO_WARN)

# gcc optimization for now also debug with -O2 like
# in Kernel (Can override in CONFIG_OPTIMIZE_FLAGS = )
OPTIMIZE_FLAGS = -O2 $(CONFIG_OPTIMIZE_FLAGS)

# Optional debug mode
ifeq ($(DEBUG), 1)
CDEBUG_FLAGS = -g -ggdb $(CONFIG_CDEBUG_FLAGS)
endif

CFLAGS = -fPIC -pthread -std=gnu11 $(CONFIG_CFLAGS)	\
	$(C_INCLUDE) $(C_DEFINE)			\
	$(OPTIMIZE_FLAGS) $(CDEBUG_FLAGS)		\
	$(CWARN) $(CNO_WARN)				\
	$(CONFIG_PEDANTIC_FLAGS)

export CFLAGS

# On Linux
CFLAGS += "-DKERNEL=0"

# List of -L -l libs
C_LIBS = -lrt -lcurses -lc -luuid -lunwind $(CONFIG_C_LIBS)

# Targets
ALL = zusd libzus.so
all: $(DEPEND) $(ALL)

clean:
	rm -vf $(LINKED_HEADERS) $(DEPEND) $(ALL) $(zusd_OBJ) $(libzus_OBJ)

# =========== Headers from the running Kernel ==================================
ZUS_API_H=zus_api.h md_def.h md.h
LINUX_STAT_H=linux/stat.h
LINKED_HEADERS=$(ZUS_API_H) $(LINUX_STAT_H)

$(ZUS_API_H):
	ln -sTf $(shell realpath --relative-to=$(ROOT) \
		  $(ZUF_KERN_DIR))/fs/zuf/$@ $@

$(LINUX_STAT_H):
	mkdir -p linux/ ;					\
	ln -sTf $(shell realpath --relative-to=$(ROOT)/linux \
		  $(ZUF_KERN_DIR))/include/uapi/$(LINUX_STAT_H) $(LINUX_STAT_H)

# ============== sub-projects===================================================
-include fs/Makefile


# ============== libzus & zusd =================================================
LIBZUS = libzus.so

libzus_OBJ += zus-core.o zus-vfs.o module.o md_zus.o nvml_movnt.o \
	      utils.o fs-loader.o

$(LIBZUS): $(libzus_OBJ:.o=.c)
	$(CC) -shared $(LDFLAGS) $(CFLAGS) $(C_LIBS) -o $@ $^

zusd_OBJ = main.o

zusd: $(zusd_OBJ) $(LIBZUS)
	$(CC) $(LDFLAGS) $(CFLAGS) $(C_LIBS) -o $@ $^

$(DEPEND): $(zusd_OBJ:.o=.c) $(libzus_OBJ:.o:.c)

# =============== common rules =================================================
# every thing should compile if Makefile or .config changed
MorC = Makefile fs/Makefile
ifneq ($(realpath .config),)
MorC += .config
endif

%.o: %.c $(MorC)
	$(CC) $(FS_CFLAGS) $(CFLAGS) -c -o $@ $<

#.============== dependencies genaration =======================================
$(DEPEND): $(LINKED_HEADERS)
	$(CC) -MM $(CFLAGS) $^ > $@

ifneq (clean, $(MAKECMDGOALS))
-include $(DEPEND)
endif

#.============== install =======================================================
SERVICE = zus.service
SYSTEMD_SERVICE_DIR = /lib/systemd/system

install:
	cp -f $(SERVICE) $(SYSTEMD_SERVICE_DIR)
	systemctl daemon-reload
	systemctl stop zus
	systemctl enable zus
	systemctl start zus
