ZDIR := $(dir $(lastword $(MAKEFILE_LIST)))
CONFIG := $(ZDIR).config
ifeq ($(M),)
PROJ_DIR := $(ZDIR)
BUILD_STR := "Z"
else
PROJ_DIR := $(M)/
BUILD_STR := "M"
endif

OBJS_DIR := $(PROJ_DIR)objs
OBJS := $(addprefix $(OBJS_DIR)/, $(strip $(PROJ_OBJS)))
OBJS_DEPS := $(OBJS:.o=.d)

-include $(CONFIG)

# What to warn about
CWARNS := error all
CWARNS += write-strings missing-prototypes undef cast-qual
CWARNS += missing-declarations nested-externs strict-prototypes
CWARNS += bad-function-cast cast-align old-style-definition extra
CWARNS += unused shadow float-equal comment sign-compare address
CWARNS += redundant-decls missing-include-dirs unknown-pragmas
CWARNS += parentheses sequence-point unused-macros endif-labels
CWARNS += overlength-strings unreachable-code missing-field-initializers
CWARNS += aggregate-return declaration-after-statement init-self
CWARNS += switch-default switch switch-enum
CWARNS += frame-larger-than=4096 larger-than=4096
CWARNS += $(PROJ_WARNS)
# Turn off some warnings
CWARNS += no-unused-parameter no-missing-field-initializers

ifeq ($(CONFIG_PEDANTIC),1)
CWARNS += format=2 sign-conversion conversion
CWARNS += strict-prototypes old-style-definition
CWARNS += pointer-arith
endif
CWARNS := -W $(addprefix -W,$(CWARNS))


CDEFS := KERNEL=0
ifdef CONFIG_ZUF_DEF_PATH
CDEFS += CONFIG_ZUF_DEF_PATH=\"$(CONFIG_ZUF_DEF_PATH)\"
endif
CDEFS += $(CONFIG_GLOBAL_CDEFS) $(PROJ_CDEFS)

ifeq ($(CONFIG_FORTIFY_SORUCE),1)
CDEFS += _FORTIFY_SOURCE=1
endif

ifeq ($(CONFIG_TRY_ANON_MMAP),1)
CDEFS += CONFIG_TRY_ANON_MMAP=1
endif

CDEFS := $(addprefix -D,$(CDEFS))

INCLUDES := $(CONFIG_GLOBAL_INCLUDES) $(PROJ_INCLUDES)
INCLUDES := $(addprefix -I,$(INCLUDES))

CFLAGS := -std=gnu11 -MMD -pthread $(CONFIG_GLOBAL_CFLAGS) $(PROJ_CFLAGS)
ifeq ($(DEBUG), 1)
CFLAGS += -g -ggdb
endif
# gcc optimization for now also debug with -O2 like
# in Kernel (Can override in CONFIG_OPTIMIZE_FLAGS = )
ifdef CONFIG_OPTIMIZE_LEVEL
CFLAGS += -O$(CONFIG_OPTIMIZE_LEVEL)
else
CFLAGS += -O2
endif

ifeq ($(CONFIG_PEDANTIC),1)
CFLAGS += -pedantic
endif

CFLAGS += $(CWARNS) $(CDEFS) $(INCLUDES)

LIBS := $(CONFIG_GLOBAL_LIBS) $(PROJ_LIBS)
LIB_DIRS := $(CONFIG_GLOBAL_LIB_DIRS) $(PROJ_LIB_DIRS)
LDFLAGS += $(CONFIG_GLOBAL_LDFLAGS) $(PROJ_LDFLAGS) -Wl,--no-undefined
LDFLAGS += $(addprefix -L,$(LIB_DIRS))
LDFLAGS += $(addprefix -l,$(LIBS))

# =============== common rules =================================================
PROJ_OBJS_DEPS += $(ZDIR)/Makefile $(ZDIR)/common.zus.mk
ifneq ($(realpath $(CONFIG)),)
PROJ_OBJS_DEPS += $(CONFIG)
endif

$(OBJS_DIR)/%.o: $(PROJ_DIR)%.c $(PROJ_OBJS_DEPS)
	$(eval BUILD_CMD := $(CC) $(CFLAGS) -c $< -o $@)
	@mkdir -p $(dir $@)
ifeq ($(CONFIG_BUILD_VERBOSE),1)
	$(BUILD_CMD)
else
	@echo "CC [$(BUILD_STR)] $(notdir $@)"
	@$(BUILD_CMD)
endif

$(PROJ_TARGET): $(PROJ_TARGET_DEPS) $(OBJS)
	$(eval LINK_CMD := $(CC) $(LDFLAGS) $(OBJS) -o $(PROJ_TARGET))
ifeq ($(CONFIG_BUILD_VERBOSE),1)
	$(LINK_CMD)
else
	@echo "LD [$(BUILD_STR)] $(notdir $(PROJ_TARGET))"
	@$(LINK_CMD)
endif

__clean: $(PROJ_CLEAN_DEPS)
	@rm -f $(OBJS_DEPS) $(PROJ_TARGET) $(OBJS)

-include $(OBJS_DEPS)

.DEFAULT_GOAL := $(PROJ_TARGET)
.PHONY: __clean
