PROJECT_ROOT := $(CURDIR)

PLATFORM ?= linux-x64
SUPPORTED_PLATFORM := linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64

LINK_MODE ?= dynamic
SUPPORTED_LINK_MODE := dynamic static

NON_BUILD_GOALS := clean dev-clean docs-assets docs serve
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
endif

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/$(PLATFORM)/obj
BIN_DIR := $(BUILD_DIR)/$(PLATFORM)/bin

COMMON_SRC := \
	src/main.c \
	src/entry.c \
	src/options.c \
	src/commands.c \
	src/state.c \
	src/filesystem.c \
	src/manifest.c \
	src/info.c \
	src/registry.c \
	src/fetch.c \
	src/ca_bundle.c \
	src/package_archive.c \
	src/extract.c \
	src/path.c \
	src/util.c \
	src/interrupt.c \
	src/platform.c

ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64),)
    CC = gcc
    TARGET := $(BIN_DIR)/cup
    SYSTEM_SRC := src/system_posix.c
    DEPS_PREFIX ?= $(HOME)/deps/$(PLATFORM)/install

    CPPFLAGS += -I$(PROJECT_ROOT)/include -DCUP_USE_EMBEDDED_CA_BUNDLE
    CFLAGS += -Wall -Wextra -Werror -std=c11 -g -D_POSIX_C_SOURCE=200809L

    ifeq ($(LINK_MODE),static)
        CPPFLAGS += -I$(DEPS_PREFIX)/include -DCUP_USE_OPENSSL_INIT
        LDFLAGS += -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64 -static

        CURL_LIBS := $(shell $(DEPS_PREFIX)/bin/curl-config --static-libs 2>/dev/null)
        ARCHIVE_LIBS := $(shell PKG_CONFIG_PATH=$(DEPS_PREFIX)/lib/pkgconfig pkg-config --static --libs libarchive 2>/dev/null)
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
    CFLAGS += -Wall -Wextra -Werror -std=c11 -g -D_DARWIN_C_SOURCE

    ifeq ($(LINK_MODE),static)
        CPPFLAGS += -I$(DEPS_PREFIX)/include -DCUP_USE_OPENSSL_INIT
        LDFLAGS += -L$(DEPS_PREFIX)/lib

        CURL_LIBS := $(shell $(DEPS_PREFIX)/bin/curl-config --static-libs 2>/dev/null)
        ARCHIVE_LIBS := $(shell PKG_CONFIG_PATH=$(DEPS_PREFIX)/lib/pkgconfig pkg-config --static --libs libarchive 2>/dev/null)
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
	CFLAGS += -Wall -Wextra -Werror -std=c11 -g

	ifeq ($(LINK_MODE),static)
		CPPFLAGS += -I$(DEPS_PREFIX)/include -DCURL_STATICLIB
		LDFLAGS += -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64 -static

		CURL_LIBS := $(shell $(DEPS_PREFIX)/bin/curl-config --static-libs 2>/dev/null)
		ARCHIVE_LIBS := $(shell PKG_CONFIG_PATH=$(DEPS_PREFIX)/lib/pkgconfig pkg-config --static --libs libarchive 2>/dev/null)
		LDLIBS += $(CURL_LIBS) $(ARCHIVE_LIBS) -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -liphlpapi -lsecur32
	else
		LDLIBS += -lcurl -larchive
	endif
endif

SRC := $(COMMON_SRC) $(SYSTEM_SRC)
OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC))

MDBOOK := $(if $(wildcard ./mdbook),./mdbook,mdbook)

.PHONY: all clean dev-clean docs-assets docs serve

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

dev-clean: clean
	rm -rf ~/.cup
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