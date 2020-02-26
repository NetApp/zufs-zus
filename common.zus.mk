ZDIR := $(dir $(lastword $(MAKEFILE_LIST)))
CONFIG := $(ZDIR).config
ifeq ($(M),)
PROJ_DIR := $(ZDIR)
BUILD_STR := Z
else
PROJ_DIR := $(M)/
BUILD_STR := M
endif

OBJS_DIR := $(PROJ_DIR)objs
OBJS := $(addprefix $(OBJS_DIR)/, $(strip $(PROJ_OBJS)))
OBJS_DEPS := $(OBJS:.o=.d)

ifeq ($(PROJ_NAME),)
$(error no name for project)
endif

ifeq ($(PROJ_LANG),)
PROJ_LANG=C
endif

ifeq ($(filter $(PROJ_LANG),C CPP),)
$(error Unknown ZUS project languege $(PROJ_LANG))
endif

-include $(CONFIG)

# What to warn about
CWARNS := error all write-strings undef cast-qual missing-declarations
CWARNS += cast-align extra unused shadow float-equal comment sign-compare
CWARNS += address redundant-decls missing-include-dirs unknown-pragmas
CWARNS += parentheses sequence-point unused-macros endif-labels
CWARNS += overlength-strings unreachable-code missing-field-initializers
CWARNS += aggregate-return init-self switch-default switch switch-enum
CWARNS += frame-larger-than=4096 larger-than=4096

ifeq ($(PROJ_LANG), C)
CWARNS += missing-prototypes nested-externs bad-function-cast
CWARNS += old-style-definition strict-prototypes declaration-after-statement
endif

CWARNS += $(PROJ_WARNS)
# Turn off some warnings
CWARNS += no-unused-parameter no-missing-field-initializers
# Turn off clang-specific warnings we don't care about
ifeq ($(CC),clang)
CWARNS += no-gnu-variable-sized-type-not-at-end
CWARNS += no-address-of-packed-member
CWARNS += no-cast-align
CWARNS += no-unused-function
endif

ifeq ($(CONFIG_PEDANTIC),1)
CWARNS += format=2 sign-conversion conversion
CWARNS += strict-prototypes old-style-definition
CWARNS += pointer-arith
endif
CWARNS := -W $(addprefix -W,$(CWARNS))

CDEFS += $(CONFIG_GLOBAL_CDEFS) $(PROJ_CDEFS)
CDEFS := $(addprefix -D,$(CDEFS))

INCLUDES := $(realpath $(CONFIG_GLOBAL_INCLUDES) $(PROJ_INCLUDES))
INCLUDES := $(addprefix -I,$(INCLUDES))

CFLAGS := -MMD $(CONFIG_GLOBAL_CFLAGS) $(PROJ_CFLAGS)
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

ifeq ($(PROJ_LANG),CPP)
CC=g++
SRC_SUFFIX := cpp
CFLAGS := -std=gnu++0x $(CFLAGS)
else
SRC_SUFFIX := c
CFLAGS := -std=gnu11 $(CFLAGS)
endif

ifeq ($(CONFIG_PEDANTIC),1)
CFLAGS += -pedantic
endif

CFLAGS += $(CWARNS) $(CDEFS) $(INCLUDES)

LIBS := $(CONFIG_GLOBAL_LIBS) $(PROJ_LIBS)
LIB_DIRS := $(realpath $(CONFIG_GLOBAL_LIB_DIRS) $(PROJ_LIB_DIRS))
LDFLAGS += $(CONFIG_GLOBAL_LDFLAGS) $(PROJ_LDFLAGS) -Wl,--no-undefined
LDFLAGS += $(addprefix -L,$(LIB_DIRS))
LDFLAGS += $(addprefix -l,$(LIBS))

ifeq ($(PROJ_TARGET_TYPE),)
PROJ_TARGET_TYPE := exec
endif

ifeq ($(filter $(PROJ_TARGET_TYPE),exec lib),)
$(error Invalid project type $(PROJ_TARGET_TYPE))
endif

ifeq ($(PROJ_TARGET_TYPE),lib)
TARGET := $(PROJ_DIR)/lib$(PROJ_NAME).so
CFLAGS := -fpic $(CFLAGS)
LDFLAGS := -shared $(LDFLAGS)
else
TARGET := $(PROJ_DIR)/$(PROJ_NAME)
endif

# =============== common rules =================================================
PROJ_OBJS_DEPS += $(ZDIR)/Makefile $(ZDIR)/common.zus.mk
ifneq ($(realpath $(CONFIG)),)
PROJ_OBJS_DEPS += $(CONFIG)
endif

ifneq ($(CONFIG_BUILD_VERBOSE),1)
	Q := @
endif

$(OBJS_DIR)/%.o: $(PROJ_DIR)%.$(SRC_SUFFIX) $(PROJ_OBJS_DEPS)
	@mkdir -p $(dir $@)
	$(if $(Q),@echo "CC [$(BUILD_STR)] $(notdir $@)")
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(TARGET_DEPS) $(OBJS)
	$(if $(Q),@echo "LD [$(BUILD_STR)] $(notdir $(TARGET))")
	$(Q)$(CC) $(OBJS) $(LDFLAGS) -o $(TARGET)
	@echo

__clean: $(PROJ_CLEAN_DEPS)
	@rm -f $(OBJS_DEPS) $(TARGET) $(OBJS)

-include $(OBJS_DEPS)

.DEFAULT_GOAL := $(TARGET)
.PHONY: __clean
