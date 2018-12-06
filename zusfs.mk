include $(M)/Makefile
ZDIR ?=$(CURDIR)
MAKEFLAGS := --no-print-directory

PROJ_TARGET := $(M)/lib$(ZM_NAME).so
PROJ_OBJS := $(ZM_OBJS)
PROJ_CDEFS := $(ZM_CDEFS)
PROJ_WARNS := $(ZM_WARNS)
PROJ_INCLUDES := $(ZM_INCLUDES) $(ZDIR)
PROJ_LIBS := zus $(ZM_LIBS) pthread
PROJ_LIB_DIRS := $(ZDIR)
PROJ_CFLAGS := -fpic $(ZM_CFLAGS)
PROJ_LDFLAGS := -shared -Wl,-Tzus_ddbg.ld $(ZM_LDFLAGS)
PROJ_OBJS_DEPS := $(M)/Makefile $(ZM_OBJS_DEPS) $(ZDIR)/zusfs.mk
PROJ_TARGET_DEPS += $(ZDIR)/libzus.so $(ZM_TARGET_DEPS)

module:
ifneq ($(ZM_PRE_BUILD),)
	@$(MAKE) -C $(M) $(ZM_PRE_BUILD)
endif
	@$(MAKE) M=$(M) -C .
ifneq ($(ZM_POST_BUILD),)
	@$(MAKE) -C $(M) $(ZM_POST_BUILD)
endif

module_clean:
ifneq ($(ZM_PRE_CLEAN),)
	@$(MAKE) -C $(M) $(ZM_PRE_CLEAN)
endif
	@$(MAKE) M=$(M) -C .  __clean
ifneq ($(ZM_POST_CLEAN),)
	@$(MAKE) -C $(M) $(ZM_POST_CLEAN)
endif

.PHONY: module module_clean

include common.zus.mk
