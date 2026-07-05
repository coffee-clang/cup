PROJECT_ROOT := $(CURDIR)

PLATFORM ?= linux-x64
SUPPORTED_PLATFORM := linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64

LINK_MODE ?= dynamic
SUPPORTED_LINK_MODE := dynamic static

BUILD_MODE ?= development
SUPPORTED_BUILD_MODE := development release

RELEASE_BUILD ?= 0

NON_BUILD_GOALS := clean reset-dev-home docs-assets docs serve version release-metadata validate-release test test-posix test-integration test-unit test-release test-windows update-ca-bundle
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
GENERATED_DIR := $(CONFIG_DIR)/generated
MODE_STAMP := $(CONFIG_DIR)/.build-mode-$(BUILD_MODE)
VERSION_STAMP := $(GENERATED_DIR)/.version-stamp
VERSION_HEADER := $(GENERATED_DIR)/version.h
VERSION_RESOURCE := $(GENERATED_DIR)/version.rc
VERSION_METADATA := $(GENERATED_DIR)/release.txt
CA_BUNDLE_STAMP := $(GENERATED_DIR)/.ca-bundle-stamp
CA_BUNDLE_HEADER := $(GENERATED_DIR)/ca_bundle.h
CA_BUNDLE_SOURCE := $(GENERATED_DIR)/ca_bundle.c

COMMON_SRC := \
    src/main.c \
    src/entrypoints.c \
    src/commands_update.c \
    src/self_update.c \
    src/entry.c \
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
    src/sha256.c \
    src/bootstrap.c \
    src/package.c \
    src/transaction.c \
    src/text.c \
    src/registry.c \
    src/fetch.c \
    src/package_archive.c \
    src/extract.c \
    src/path.c \
    src/interrupt.c \
    src/platform.c

CPPFLAGS += -I$(GENERATED_DIR) -I$(PROJECT_ROOT)/include -DCUP_USE_EMBEDDED_CA_BUNDLE
CFLAGS += -Wall -Wextra -Werror -std=c11

ifeq ($(BUILD_MODE),release)
    CFLAGS += -O2 -DNDEBUG
else
    CFLAGS += -O0 -g3
endif

ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64 macos-x64 macos-arm64),)
    SYSTEM_SRC := src/system_posix.c
    TARGET := $(BIN_DIR)/cup
    DEPS_PREFIX ?= $(HOME)/deps/$(PLATFORM)/install
endif

ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64),)
    CC := gcc
    CFLAGS += -D_POSIX_C_SOURCE=200809L
endif

ifneq ($(filter $(PLATFORM),macos-x64 macos-arm64),)
    CC := clang
    CFLAGS += -D_DARWIN_C_SOURCE
endif

ifeq ($(PLATFORM),windows-x64)
    CC := x86_64-w64-mingw32-gcc
    WINDRES := x86_64-w64-mingw32-windres
    SYSTEM_SRC := src/system_windows.c
    TARGET := $(BIN_DIR)/cup.exe
    DEPS_PREFIX ?= $(HOME)/deps/windows-x64/install
    RESOURCE_OBJ := $(OBJ_DIR)/version-resource.o
endif

CPPFLAGS += -I$(DEPS_PREFIX)/include
LDFLAGS += -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64

ifeq ($(LINK_MODE),static)
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
    LDLIBS += -largtable3 $(CURL_LIBS) $(ARCHIVE_LIBS)

    ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64),)
        CPPFLAGS += -DCUP_USE_OPENSSL_INIT
        LDFLAGS += -static
        LDLIBS += -ldl -pthread
    endif

    ifneq ($(filter $(PLATFORM),macos-x64 macos-arm64),)
        CPPFLAGS += -DCUP_USE_OPENSSL_INIT
    endif

    ifeq ($(PLATFORM),windows-x64)
        CPPFLAGS += -DCURL_STATICLIB
        LDFLAGS += -static
        LDLIBS += -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -liphlpapi -lsecur32
    endif
else
    LDLIBS += -largtable3 -lcurl -larchive
endif

SRC := $(COMMON_SRC) $(SYSTEM_SRC)
CA_BUNDLE_OBJ := $(OBJ_DIR)/ca_bundle.o
OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC)) $(CA_BUNDLE_OBJ) $(RESOURCE_OBJ)
DEP := $(filter %.d,$(OBJ:.o=.d))
MDBOOK := $(if $(wildcard ./mdbook),./mdbook,mdbook)

.PHONY: all clean reset-dev-home docs-assets docs serve version release-metadata validate-release test test-posix test-integration test-unit test-release test-windows update-ca-bundle FORCE

all: $(TARGET)

FORCE:

$(VERSION_STAMP): FORCE VERSION scripts/version.sh
	@mkdir -p $(GENERATED_DIR)
	@CUP_RELEASE_BUILD='$(RELEASE_BUILD)' \
		./scripts/version.sh generate '$(GENERATED_DIR)'
	@touch $@

$(CA_BUNDLE_STAMP): certs/cacert.pem scripts/certs/generate-ca-bundle.sh
	@mkdir -p $(GENERATED_DIR)
	@./scripts/certs/generate-ca-bundle.sh certs/cacert.pem $(GENERATED_DIR)
	@touch $@

$(CA_BUNDLE_HEADER) $(CA_BUNDLE_SOURCE): $(CA_BUNDLE_STAMP)
	@test -f $@ || { \
		rm -f $(CA_BUNDLE_STAMP); \
		$(MAKE) $(CA_BUNDLE_STAMP); \
	}

$(MODE_STAMP):
	@mkdir -p $(CONFIG_DIR)
	@rm -f $(CONFIG_DIR)/.build-mode-*
	@touch $@

$(OBJ): | $(VERSION_STAMP) $(CA_BUNDLE_STAMP)

$(TARGET): $(OBJ) | $(MODE_STAMP)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(CA_BUNDLE_OBJ): $(CA_BUNDLE_SOURCE) $(CA_BUNDLE_HEADER) $(MODE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $(CA_BUNDLE_SOURCE) -o $@

$(OBJ_DIR)/%.o: src/%.c $(MODE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

ifeq ($(PLATFORM),windows-x64)
$(RESOURCE_OBJ): $(VERSION_RESOURCE) $(MODE_STAMP)
	@mkdir -p $(dir $@)
	$(WINDRES) -I$(GENERATED_DIR) $< -O coff -o $@
endif

-include $(DEP)

version: $(VERSION_STAMP)
	@cat $(VERSION_METADATA)

validate-release:
	@CUP_RELEASE_BUILD=1 ./scripts/version.sh validate-release

release-metadata: $(VERSION_STAMP)
	@printf '%s\n' '$(VERSION_METADATA)'

clean:
	@rm -rf $(BUILD_DIR)

reset-dev-home:
	@test "$(CUP_ALLOW_DEV_CLEAN)" = "1" || { \
		echo "Refusing to remove $(HOME)/.cup without CUP_ALLOW_DEV_CLEAN=1" >&2; \
		exit 1; \
	}
	@case "$(HOME)" in \
		/*) test "$(HOME)" != "/" ;; \
		*) false ;; \
	esac || { echo "Invalid HOME for reset-dev-home" >&2; exit 1; }
	@rm -rf $(BUILD_DIR)
	@rm -rf -- "$(HOME)/.cup"
	@clear 2>/dev/null || true

test: test-unit test-posix

test-unit:
	@./scripts/tests/unit.sh

test-posix: test-integration

test-integration:
	@./scripts/tests/integration.sh

test-release:
	@test -n "$(RELEASE_DIR)" || { echo "Set RELEASE_DIR=<candidate-dir>" >&2; exit 2; }
	@./scripts/tests/release.sh "$(RELEASE_DIR)"

test-windows:
	@test -f build/windows-x64/dynamic/bin/cup.exe || { \
		echo "Build the Windows development binary before running native tests." >&2; \
		exit 1; \
	}
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
		"$$(cygpath -w '$(PROJECT_ROOT)/scripts/tests/integration/windows/run.ps1')" \
		-CupPath "$$(cygpath -w '$(PROJECT_ROOT)/build/windows-x64/dynamic/bin/cup.exe')"

update-ca-bundle:
	@./scripts/certs/update-ca-bundle.sh

docs-assets:
	@echo "Fetching remote docs theme assets..."
	@./scripts/fetch-docs-assets.sh

docs: docs-assets
	@echo "Building docs website..."
	@$(MDBOOK) build
	@echo "Docs built to book/"

serve: docs-assets
	$(MDBOOK) serve
