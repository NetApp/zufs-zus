include $(M)/Makefile
ZDIR ?=$(CURDIR)
MAKEFLAGS := --no-print-directory

PROJ_NAME := $(ZM_NAME)
PROJ_OBJS := $(ZM_OBJS)
PROJ_CDEFS := $(ZM_CDEFS)
PROJ_WARNS := $(ZM_WARNS)
PROJ_INCLUDES := $(ZM_INCLUDES) $(ZDIR)
PROJ_LIBS := $(ZM_LIBS)
PROJ_LIB_DIRS := $(ZDIR) $(ZM_LIB_DIRS)
PROJ_CFLAGS := $(ZM_CFLAGS)
PROJ_LDFLAGS := $(ZM_LDFLAGS)
PROJ_OBJS_DEPS := $(M)/Makefile $(ZM_OBJS_DEPS) $(ZDIR)/zusmodule.mk
PROJ_TARGET_DEPS += $(ZM_TARGET_DEPS)

# ZM_TYPE==GENERIC means a generic binary that isn't a file system library
# but still depends on libzus.
# ZM_TYPE==STANDALONE means a binary that does not require libzus.
# All other modules are assumed to be a filesystem library.
ifeq ($(strip $(ZM_TYPE)),GENERIC)
PROJ_LIBS += zus pthread
PROJ_TARGET_DEPS += $(ZDIR)/libzus.so
else ifeq ($(strip $(ZM_TYPE)),STANDALONE)
PROJ_TARGET_TYPE := $(ZM_TARGET_TYPE)
else # ZM_TYPE==LIB
PROJ_TARGET_TYPE := lib
PROJ_LDFLAGS := -Wl,-Tzus_ddbg.ld $(PROJ_LDFLAGS)
PROJ_LIBS += zus pthread
PROJ_TARGET_DEPS += $(ZDIR)/libzus.so
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
