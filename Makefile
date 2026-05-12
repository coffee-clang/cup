PROJECT_ROOT := $(CURDIR)

PLATFORM ?= linux-x64

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/$(PLATFORM)/obj
BIN_DIR := $(BUILD_DIR)/$(PLATFORM)/bin

COMMON_SRC := \
	src/main.c \
	src/options.c \
	src/commands.c \
	src/state.c \
	src/filesystem.c \
	src/manifest.c \
	src/registry.c \
	src/fetch.c \
	src/extract.c \
	src/util.c \
	src/interrupt.c \
	src/platform.c

ifeq ($(PLATFORM),linux-x64)
	CC = gcc
	TARGET := $(BIN_DIR)/cup
	SYSTEM_SRC := src/system_posix.c
	DEPS_PREFIX ?= $(HOME)/deps/linux-x64/install

	CPPFLAGS += -I$(DEPS_PREFIX)/include -I$(PROJECT_ROOT)/include
	CFLAGS += -Wall -Wextra -Werror -std=c11 -g -D_POSIX_C_SOURCE=200809L
	LDFLAGS += -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64 -static

	CURL_LIBS := $(shell $(DEPS_PREFIX)/bin/curl-config --static-libs 2>/dev/null)
	ARCHIVE_LIBS := $(shell PKG_CONFIG_PATH=$(DEPS_PREFIX)/lib/pkgconfig pkg-config --static --libs libarchive 2>/dev/null)
	LDLIBS += $(CURL_LIBS) $(ARCHIVE_LIBS) -ldl -pthread
endif

ifeq ($(PLATFORM),windows-x64)
	CC = x86_64-w64-mingw32-gcc
	TARGET := $(BIN_DIR)/cup.exe
	SYSTEM_SRC := src/system_windows.c
	DEPS_PREFIX ?= $(HOME)/deps/windows-x64/install

	CPPFLAGS += -I$(DEPS_PREFIX)/include -I$(PROJECT_ROOT)/include -DCURL_STATICLIB
	CFLAGS += -Wall -Wextra -Werror -std=c11 -g
	LDFLAGS += -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64 -static

	CURL_LIBS := $(shell $(DEPS_PREFIX)/bin/curl-config --static-libs 2>/dev/null)
	ARCHIVE_LIBS := $(shell PKG_CONFIG_PATH=$(DEPS_PREFIX)/lib/pkgconfig pkg-config --static --libs libarchive 2>/dev/null)
	LDLIBS += $(CURL_LIBS) $(ARCHIVE_LIBS) -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -liphlpapi -lsecur32
endif

ifndef CC
$(error Unsupported PLATFORM '$(PLATFORM)'. Supported values: linux-x64 windows-x64)
endif

SRC := $(COMMON_SRC) $(SYSTEM_SRC)
OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC))

.PHONY: all clean dev-clean

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
	rm -rf ./error-output.txt
	clear