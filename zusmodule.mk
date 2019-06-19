include $(M)/Makefile
ZDIR ?=$(CURDIR)
MAKEFLAGS := --no-print-directory

PROJ_NAME := $(ZM_NAME)
PROJ_OBJS := $(ZM_OBJS)
PROJ_CDEFS := $(ZM_CDEFS)
PROJ_WARNS := $(ZM_WARNS)
PROJ_INCLUDES := $(ZM_INCLUDES)
PROJ_LIBS := $(ZM_LIBS)
PROJ_LIB_DIRS := $(ZM_LIB_DIRS)
PROJ_CFLAGS := $(ZM_CFLAGS)
PROJ_LDFLAGS := $(ZM_LDFLAGS)
PROJ_OBJS_DEPS := $(M)/Makefile $(ZM_OBJS_DEPS) $(ZDIR)/zusmodule.mk
PROJ_TARGET_DEPS += $(ZM_TARGET_DEPS)

# ZM_TYPE can be one of the following:
# FS means a ZUS filesystem libraray
# ZUS_BIN means a binary that isn't a file system library but
# still depends on libzus.
# ZM_TYPE==GENERIC means a binary that does not require libzus.
ifeq ($(ZM_TYPE),)
ZM_TYPE := FS # default to ZUS_FS
endif
ZM_TYPE := $(strip $(ZM_TYPE))

ifeq ($(filter $(ZM_TYPE),FS ZUS_BIN GENERIC),)
$(error Unknown ZUS projec type $(ZM_TYPE))
endif

ifeq ($(filter $(ZM_TYPE),FS ZUS_BIN), $(ZM_TYPE))
PROJ_INCLUDES += $(ZDIR)
PROJ_LIB_DIRS += $(ZDIR)
PROJ_LIBS += zus
PROJ_CFLAGS := -pthread $(PROJ_CFLAGS)
PROJ_LDFLAGS := -pthread $(PROJ_LDFLAGS)
PROJ_TARGET_DEPS += $(ZDIR)/libzus.so
ifeq ($(ZM_TYPE), FS)
PROJ_TARGET_TYPE := lib
PROJ_LDFLAGS := -Wl,-Tzus_ddbg.ld $(PROJ_LDFLAGS)
endif
else # Generic binary
PROJ_TARGET_TYPE := $(ZM_TARGET_TYPE)
endif

module:
	@$(foreach t,$(ZM_PRE_BUILD),$(MAKE) -C $(M) $(t);)
	@$(MAKE) M=$(M) -C .
	@$(foreach t,$(ZM_POST_BUILD),$(MAKE) -C $(M) $(t);)

module_clean:
	@$(foreach t,$(ZM_PRE_CLEAN),$(MAKE) -C $(M) $(t);)
	@$(MAKE) M=$(M) -C .  __clean
	@$(foreach t,$(ZM_POST_CLEAN),$(MAKE) -C $(M) $(t);)

.PHONY: module module_clean

include common.zus.mk
