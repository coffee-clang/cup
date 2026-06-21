PROJECT_ROOT := $(CURDIR)

PLATFORM ?= linux-x64
SUPPORTED_PLATFORM := linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64

LINK_MODE ?= dynamic
SUPPORTED_LINK_MODE := dynamic static

BUILD_MODE ?= development
SUPPORTED_BUILD_MODE := development release

NON_BUILD_GOALS := clean reset-dev-home docs-assets docs serve
ifeq ($(strip $(MAKECMDGOALS)),)
    NEED_BUILD_CONFIG := 1
else ifneq ($(strip $(filter-out $(NON_BUILD_GOALS),$(MAKECMDGOALS))),)
    NEED_BUILD_CONFIG := 1
else
    NEED_BUILD_CONFIG := 0
endif

ifeq ($(NEED_BUILD_CONFIG),1)
    ifeq ($(filter $(PLATFORM),$(SUPPORTED_PLATFORM)),)
        $(error Unsupported PLATFORM '$(PLATFORM)'. Supported values: $(SUPPORTED_PLATFORM))
    endif

    ifeq ($(filter $(LINK_MODE),$(SUPPORTED_LINK_MODE)),)
        $(error Unsupported LINK_MODE '$(LINK_MODE)'. Supported values: $(SUPPORTED_LINK_MODE))
    endif

    ifeq ($(filter $(BUILD_MODE),$(SUPPORTED_BUILD_MODE)),)
        $(error Unsupported BUILD_MODE '$(BUILD_MODE)'. Supported values: $(SUPPORTED_BUILD_MODE))
    endif
endif

BUILD_DIR := build
CONFIG_DIR := $(BUILD_DIR)/$(PLATFORM)/$(LINK_MODE)
OBJ_DIR := $(CONFIG_DIR)/obj
BIN_DIR := $(CONFIG_DIR)/bin
MODE_STAMP := $(CONFIG_DIR)/.build-mode-$(BUILD_MODE)

COMMON_SRC := \
    src/main.c \
    src/entry.c \
    src/options.c \
    src/command_context.c \
    src/commands_install.c \
    src/commands_remove.c \
    src/commands_state.c \
    src/doctor.c \
    src/repair.c \
    src/uninstall.c \
    src/state.c \
    src/filesystem.c \
    src/layout.c \
    src/manifest.c \
    src/info.c \
    src/checksum.c \
    src/bootstrap.c \
    src/package.c \
    src/transaction.c \
    src/text.c \
    src/registry.c \
    src/fetch.c \
    src/ca_bundle.c \
    src/package_archive.c \
    src/extract.c \
    src/path.c \
    src/interrupt.c \
    src/platform.c


ifeq ($(BUILD_MODE),release)
    CFLAGS += -O2 -DNDEBUG
else
    CFLAGS += -O0 -g3
endif

ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64),)
    CC = gcc
    TARGET := $(BIN_DIR)/cup
    SYSTEM_SRC := src/system_posix.c
    DEPS_PREFIX ?= $(HOME)/deps/$(PLATFORM)/install

    CPPFLAGS += -I$(PROJECT_ROOT)/include -DCUP_USE_EMBEDDED_CA_BUNDLE
    CFLAGS += -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L

    ifeq ($(LINK_MODE),static)
        CPPFLAGS += -I$(DEPS_PREFIX)/include -DCUP_USE_OPENSSL_INIT
        LDFLAGS += -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64 -static

        CURL_CONFIG := $(DEPS_PREFIX)/bin/curl-config
        ifeq ($(wildcard $(CURL_CONFIG)),)
            $(error Missing $(CURL_CONFIG). Build the static dependencies first)
        endif
        CURL_LIBS := $(shell $(CURL_CONFIG) --static-libs 2>/dev/null)
        ARCHIVE_LIBS := $(shell PKG_CONFIG_PATH=$(DEPS_PREFIX)/lib/pkgconfig:$(DEPS_PREFIX)/lib64/pkgconfig pkg-config --static --libs libarchive 2>/dev/null)
        ifeq ($(strip $(CURL_LIBS)),)
            $(error curl-config did not return static link flags)
        endif
        ifeq ($(strip $(ARCHIVE_LIBS)),)
            $(error pkg-config did not return static libarchive link flags)
        endif
        LDLIBS += $(CURL_LIBS) $(ARCHIVE_LIBS) -ldl -pthread
    else
        LDLIBS += -lcurl -larchive
    endif
endif

ifneq ($(filter $(PLATFORM),macos-x64 macos-arm64),)
    CC = clang
    TARGET := $(BIN_DIR)/cup
    SYSTEM_SRC := src/system_posix.c
    DEPS_PREFIX ?= $(HOME)/deps/$(PLATFORM)/install

    CPPFLAGS += -I$(PROJECT_ROOT)/include -DCUP_USE_EMBEDDED_CA_BUNDLE
    CFLAGS += -Wall -Wextra -Werror -std=c11 -D_DARWIN_C_SOURCE

    ifeq ($(LINK_MODE),static)
        CPPFLAGS += -I$(DEPS_PREFIX)/include -DCUP_USE_OPENSSL_INIT
        LDFLAGS += -L$(DEPS_PREFIX)/lib

        CURL_CONFIG := $(DEPS_PREFIX)/bin/curl-config
        ifeq ($(wildcard $(CURL_CONFIG)),)
            $(error Missing $(CURL_CONFIG). Build the static dependencies first)
        endif
        CURL_LIBS := $(shell $(CURL_CONFIG) --static-libs 2>/dev/null)
        ARCHIVE_LIBS := $(shell PKG_CONFIG_PATH=$(DEPS_PREFIX)/lib/pkgconfig:$(DEPS_PREFIX)/lib64/pkgconfig pkg-config --static --libs libarchive 2>/dev/null)
        ifeq ($(strip $(CURL_LIBS)),)
            $(error curl-config did not return static link flags)
        endif
        ifeq ($(strip $(ARCHIVE_LIBS)),)
            $(error pkg-config did not return static libarchive link flags)
        endif
        LDLIBS += $(CURL_LIBS) $(ARCHIVE_LIBS)
    else
        LDLIBS += -lcurl -larchive
    endif
endif

ifeq ($(PLATFORM),windows-x64)
	CC = x86_64-w64-mingw32-gcc
	TARGET := $(BIN_DIR)/cup.exe
	SYSTEM_SRC := src/system_windows.c
	DEPS_PREFIX ?= $(HOME)/deps/windows-x64/install

	CPPFLAGS += -I$(PROJECT_ROOT)/include -DCUP_USE_EMBEDDED_CA_BUNDLE
	CFLAGS += -Wall -Wextra -Werror -std=c11

	ifeq ($(LINK_MODE),static)
		CPPFLAGS += -I$(DEPS_PREFIX)/include -DCURL_STATICLIB
		LDFLAGS += -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64 -static

		CURL_CONFIG := $(DEPS_PREFIX)/bin/curl-config
		ifeq ($(wildcard $(CURL_CONFIG)),)
			$(error Missing $(CURL_CONFIG). Build the static dependencies first)
		endif
		CURL_LIBS := $(shell $(CURL_CONFIG) --static-libs 2>/dev/null)
		ARCHIVE_LIBS := $(shell PKG_CONFIG_PATH=$(DEPS_PREFIX)/lib/pkgconfig:$(DEPS_PREFIX)/lib64/pkgconfig pkg-config --static --libs libarchive 2>/dev/null)
		ifeq ($(strip $(CURL_LIBS)),)
			$(error curl-config did not return static link flags)
		endif
		ifeq ($(strip $(ARCHIVE_LIBS)),)
			$(error pkg-config did not return static libarchive link flags)
		endif
		LDLIBS += $(CURL_LIBS) $(ARCHIVE_LIBS) -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -liphlpapi -lsecur32
	else
		LDLIBS += -lcurl -larchive
	endif
endif

SRC := $(COMMON_SRC) $(SYSTEM_SRC)
OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

MDBOOK := $(if $(wildcard ./mdbook),./mdbook,mdbook)

.PHONY: all clean reset-dev-home docs-assets docs serve

all: $(TARGET)

$(MODE_STAMP):
	@mkdir -p $(CONFIG_DIR)
	@rm -f $(CONFIG_DIR)/.build-mode-*
	@touch $@

$(TARGET): $(OBJ) | $(MODE_STAMP)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.c $(MODE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

clean:
	rm -rf $(BUILD_DIR)

reset-dev-home:
	@test "$(CUP_ALLOW_DEV_CLEAN)" = "1" || { \
		echo "Refusing to remove $(HOME)/.cup without CUP_ALLOW_DEV_CLEAN=1" >&2; \
		exit 1; \
	}
	@case "$(HOME)" in \
		/*) test "$(HOME)" != "/" ;; \
		*) false ;; \
	esac || { echo "Invalid HOME for reset-dev-home" >&2; exit 1; }
	rm -rf $(BUILD_DIR)
	rm -rf -- "$(HOME)/.cup"
    clear

docs-assets:
	@echo "Fetching remote docs theme assets..."
	@./scripts/fetch-docs-assets.sh

docs: docs-assets
	@echo "Building docs website..."
	@$(MDBOOK) build
	@echo "Docs built to book/"

serve: docs-assets
	$(MDBOOK) serve