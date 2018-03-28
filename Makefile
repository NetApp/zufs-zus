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

# What to warn about
CWARN =  -W -Werror -Wall
CWARN += -Wwrite-strings -Wmissing-prototypes  -Wundef -Wcast-qual
CWARN += -Wmissing-declarations -Wnested-externs -Wstrict-prototypes
CWARN += -Wbad-function-cast -Wcast-align -Wold-style-definition -Wextra
CWARN += -Wunused -Wshadow -Wfloat-equal -Wcomment -Wsign-compare -Waddress
CWARN += -Wredundant-decls -Wmissing-include-dirs -Wunknown-pragmas -Wvla
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

# On Linux
CFLAGS += "-DKERNEL=0"

# List of -L -l libs
C_LIBS = -lrt -lcurses -lc $(CONFIG_C_LIBS)

# Targets
ALL = zus
all: $(DEPEND) $(ALL)

clean:
	rm -vf $(LINKED_HEADERS) $(DEPEND) $(ALL)  *.o fs/*.o
# =========== Headers from the running Kernel ==================================
ZUS_API_H=zus_api.h
LINUX_STAT_H=linux/stat.h
LINKED_HEADERS=$(ZUS_API_H) $(LINUX_STAT_H)

$(ZUS_API_H):
	ln -sTf $(abspath $(ZUS_API_INC)) $(ZUS_API_H)

$(LINUX_STAT_H):
	if ! grep -q STATX_ /usr/include/linux/stat.h; then		\
		mkdir -p linux/;					\
		ln -sTf $(abspath 					\
			$(ZUS_API_INC)/../../../include/uapi/$(LINUX_STAT_H)) 	\
			$(LINUX_STAT_H) ; \
	fi

# ============== sub-projects===================================================
-include fs/Makefile

# ============== zus ===========================================================
zus_OBJ = zus-core.o zus-vfs.o main.o module.o $(fs_builtin)

zus:  $(zus_OBJ)
	$(CC) $(LDFLAGS) $(CFLAGS) $(C_LIBS) -o $@ $^

$(DEPEND): $(zus_OBJ:.o=.c)

# =============== common rules =================================================
# every thing should compile if Makefile or .config changed
MorC = Makefile
ifneq ($(realpath .config),)
MorC += .config
endif

%.o: %.c $(MorC)
	$(CC) $(CFLAGS) -c -o $@ $(@:.o=.c)

#.============== dependencies genaration =======================================
$(DEPEND): $(LINKED_HEADERS)
	$(CC) -MM $(CFLAGS) $^ > $@

ifneq (clean, $(MAKECMDGOALS))
-include $(DEPEND)
endif
