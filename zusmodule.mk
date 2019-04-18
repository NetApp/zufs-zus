include $(M)/Makefile
ZDIR ?=$(CURDIR)
MAKEFLAGS := --no-print-directory

PROJ_OBJS := $(ZM_OBJS)
PROJ_CDEFS := $(ZM_CDEFS)
PROJ_WARNS := $(ZM_WARNS)
PROJ_INCLUDES := $(ZM_INCLUDES) $(ZDIR)
PROJ_LIBS := zus $(ZM_LIBS) pthread
PROJ_LIB_DIRS := $(ZDIR) $(ZM_LIB_DIRS)
PROJ_CFLAGS := $(ZM_CFLAGS)
PROJ_LDFLAGS := $(ZM_LDFLAGS)
PROJ_OBJS_DEPS := $(M)/Makefile $(ZM_OBJS_DEPS) $(ZDIR)/zusmodule.mk
PROJ_EXTRA_OBJS := $(ZM_EXTRA_OBJS)
PROJ_TARGET_DEPS += $(ZDIR)/libzus.so $(ZM_TARGET_DEPS) $(ZM_EXTRA_OBJS)

# A ZM_GENERIC==1 setting represnts a generic module that isn't a file system
# library. (tests, mkfs programs, etc.)
ifeq ($(strip $(ZM_GENERIC)),1)
PROJ_TARGET := $(M)/$(ZM_NAME)
else
PROJ_TARGET := $(M)/lib$(ZM_NAME).so
PROJ_CFLAGS := -fpic $(PROJ_CFLAGS)
PROJ_LDFLAGS := -shared -Wl,-Tzus_ddbg.ld $(PROJ_LDFLAGS)
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
