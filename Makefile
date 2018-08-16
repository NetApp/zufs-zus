# SPDX-License-Identifier: BSD-3-Clause
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
C_LIBS = -lrt -luuid -lunwind -ldl $(CONFIG_C_LIBS)

# Any libzus dependent code and any zusFS plugins in fs/XXX should
# include $(LDFLAGS) in the $(cc) -shared compilation
# We force all symbols to resolve at compile time with -Wl,--no-undefined
LDFLAGS += -Wl,-L$(CURDIR) -lzus -Wl,--no-undefined
export LDFLAGS

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

# ============== libzus & zusd =================================================
LIBZUS = libzus.so

libzus_OBJ += zus-core.o zus-vfs.o module.o md_zus.o nvml_movnt.o \
	      utils.o fs-loader.o pa.o

_LDFLAG=-Wl,--no-undefined

$(LIBZUS): $(libzus_OBJ:.o=.c)
	$(CC) -shared $(_LDFLAG) $(CFLAGS) $(C_LIBS) -o $@ $^

zusd_OBJ = main.o signals.o

zusd: $(zusd_OBJ) $(LIBZUS)
	$(CC) $(_LDFLAG) $(CFLAGS) $(C_LIBS) -o $@ $^

$(DEPEND): $(zusd_OBJ:.o=.c) $(libzus_OBJ:.o:.c)

# ============== sub-projects===================================================
-include fs/Makefile

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

#.============== install & package =============================================
SERVICE = zusd.service
SYSTEMD_SERVICE_DIR = /lib/systemd/system
SYSTEMD_DEPS_DIR = /etc/systemd/system/multi-user.target.wants
ZUFS_LIB_DIR = /usr/lib/zufs

install:
	mkdir -p $(DESTDIR){/sbin,/usr/lib64,$(ZUFS_LIB_DIR)}
	mkdir -p $(DESTDIR){$(SYSTEMD_SERVICE_DIR),$(SYSTEMD_DEPS_DIR)}
	cp -f zusd $(DESTDIR)/sbin
	cp -f $(LIBZUS) $(DESTDIR)/usr/lib64
	cp -f fs/foofs/libfoofs.so $(DESTDIR)$(ZUFS_LIB_DIR) || :
	cp -f $(SERVICE) $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	ln -sf $(SYSTEMD_SERVICE_DIR)/$(SERVICE) $(DESTDIR)$(SYSTEMD_DEPS_DIR)
	[[ -z "$(DESTDIR)" ]] && pkg/post_install.sh || :

rpm:
	$(eval TMPDIR := $(shell mktemp -d))
	$(MAKE) -C . DESTDIR=$(TMPDIR) install
	$(eval GIT_HASH := $(shell git rev-parse HEAD))
	fpm -s dir -t $@ -n zufs-zus -v $(VER) -C $(TMPDIR) \
		--iteration $(BUILD_ID) --epoch 1 \
		--url "netapp.com" --license "GPL/BSD" --vendor "NetApp Inc." \
		--description "`printf "ZUS - Zero-copy User-mode Server\nID: $(GIT_HASH)"`" \
		-d libunwind -d libuuid -d libpmem -d zufs-zuf \
		--rpm-rpmbuild-define "_build_id_links none" \
		--before-remove pkg/pre_uninstall.sh \
		--after-remove pkg/post_uninstall.sh \
		--after-install pkg/post_install.sh .
	rm -rf $(TMPDIR)

.PHONY: all clean install rpm

# =============== tags =================================================

cscope:
	cscope -bcqR

.PHONY: cscope
