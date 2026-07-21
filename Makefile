# Build selectors and supported configurations.
.DEFAULT_GOAL := all
PROJECT_ROOT := $(CURDIR)

# GNU Make treats whitespace as a list separator. The checkout itself may live
# in a path containing ordinary spaces, so paths embedded in compiler flags are
# escaped explicitly. User-selected build and dependency roots remain
# whitespace-free until all supported metadata formats can quote them portably.
empty :=
space := $(empty) $(empty)
escape_spaces = $(subst $(space),\$(space),$(1))

SUPPORTED_PLATFORM := linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64
SUPPORTED_CONFIGURATION := development debug coverage sanitizers release

# Current CI build targets are explicit so CUP and its pinned dependency graph
# agree. They are provisional compatibility baselines until native minimum-OS
# evidence is collected; they are not an unconditional long-term support claim.
CUP_MACOS_DEPLOYMENT_TARGET := 13.0
CUP_WINDOWS_WINNT := 0x0A00

HOST_SYSTEM := $(shell uname -s 2>/dev/null)
HOST_MACHINE := $(shell uname -m 2>/dev/null)
WINDOWS_MACHINE := $(strip \
    $(PROCESSOR_ARCHITEW6432) $(PROCESSOR_ARCHITECTURE) $(HOST_MACHINE))
ifeq ($(OS),Windows_NT)
    ifneq ($(filter AMD64 amd64 x86_64,$(WINDOWS_MACHINE)),)
        NATIVE_PLATFORM := windows-x64
    else
        NATIVE_PLATFORM := unsupported
    endif
else ifeq ($(HOST_SYSTEM),Linux)
    ifneq ($(filter x86_64 amd64,$(HOST_MACHINE)),)
        NATIVE_PLATFORM := linux-x64
    else ifneq ($(filter arm64 aarch64,$(HOST_MACHINE)),)
        NATIVE_PLATFORM := linux-arm64
    else
        NATIVE_PLATFORM := unsupported
    endif
else ifeq ($(HOST_SYSTEM),Darwin)
    ifneq ($(filter x86_64 amd64,$(HOST_MACHINE)),)
        NATIVE_PLATFORM := macos-x64
    else ifneq ($(filter arm64 aarch64,$(HOST_MACHINE)),)
        NATIVE_PLATFORM := macos-arm64
    else
        NATIVE_PLATFORM := unsupported
    endif
else
    NATIVE_PLATFORM := unsupported
endif

# Ignore unrelated environment variables such as PLATFORM=linux/amd64. An
# explicit command-line selector remains authoritative and is validated below.
ifeq ($(origin PLATFORM),environment)
    ifeq ($(filter $(PLATFORM),$(SUPPORTED_PLATFORM)),)
        PLATFORM := $(NATIVE_PLATFORM)
    endif
endif
PLATFORM ?= $(NATIVE_PLATFORM)

# The public build interface is target-based. These former selectors are
# intentionally rejected instead of retained as compatibility aliases.
REMOVED_BUILD_SELECTORS := \
    LINK_MODE BUILD_MODE RELEASE_BUILD CUP_RELEASE_BUILD \
    COVERAGE SANITIZERS DEBUG_SYMBOLS
ifneq ($(strip $(foreach variable,$(REMOVED_BUILD_SELECTORS),\
        $(if $(filter command\ line environment,$(origin $(variable))),$(variable)))),)
    $(error LINK_MODE, BUILD_MODE, RELEASE_BUILD, CUP_RELEASE_BUILD, COVERAGE, \
        SANITIZERS and DEBUG_SYMBOLS were removed; use target-based builds and EXTRA_* flags)
endif

# Mandatory project flags are owned by the Makefile. Command-line additions use
# the explicit EXTRA_* variables so language, warning and linkage policy cannot
# be erased accidentally. Environment values are ignored by the final override.
DIRECT_FLAG_VARIABLES := CPPFLAGS CFLAGS LDFLAGS LDLIBS
ifneq ($(strip $(foreach variable,$(DIRECT_FLAG_VARIABLES),\
        $(if $(filter command\ line,$(origin $(variable))),$(variable)))),)
    $(error Direct CPPFLAGS/CFLAGS/LDFLAGS/LDLIBS overrides are not supported; use \
        EXTRA_CPPFLAGS, EXTRA_CFLAGS, EXTRA_LDFLAGS or EXTRA_LDLIBS)
endif

# CUP_BUILD_CONFIGURATION and CUP_OFFICIAL_BUILD are internal recursive-make
# inputs used by the public targets and release workflow. They are not public
# user-facing selectors.
ifeq ($(origin CONFIGURATION),command line)
    $(error CONFIGURATION is internal; select make, debug, coverage, sanitizers or release)
endif
CUP_BUILD_CONFIGURATION ?= development
CUP_OFFICIAL_BUILD ?= 0
CONFIGURATION := $(CUP_BUILD_CONFIGURATION)

VERSION_OFFICIAL_BUILD := 0
ifeq ($(CONFIGURATION),release)
    VERSION_OFFICIAL_BUILD := $(CUP_OFFICIAL_BUILD)
endif

# Validate selectors only for targets that compile the executable.
NON_BUILD_GOALS := \
    all debug coverage sanitizers release help clean reset-dev-home deps docs-assets \
    docs serve version release-metadata validate-release test test-posix \
    test-integration test-unit test-unit-build test-helpers test-build \
    test-repository test-release test-windows test-portability-linux \
    test-coverage test-sanitizers update-ca-bundle
ifeq ($(strip $(MAKECMDGOALS)),)
    NEED_BUILD_CONFIG := 0
else ifneq ($(strip $(filter-out $(NON_BUILD_GOALS),$(MAKECMDGOALS))),)
    NEED_BUILD_CONFIG := 1
else
    NEED_BUILD_CONFIG := 0
endif

ifeq ($(NEED_BUILD_CONFIG),1)
    ifeq ($(filter $(PLATFORM),$(SUPPORTED_PLATFORM)),)
        $(error Unsupported PLATFORM '$(PLATFORM)'. Supported values: $(SUPPORTED_PLATFORM))
    endif
    ifeq ($(filter $(CONFIGURATION),$(SUPPORTED_CONFIGURATION)),)
        $(error Unsupported build configuration '$(CONFIGURATION)'. Supported values: \
            $(SUPPORTED_CONFIGURATION))
    endif
    ifneq ($(filter $(CUP_OFFICIAL_BUILD),0 1),$(CUP_OFFICIAL_BUILD))
        $(error CUP_OFFICIAL_BUILD must be 0 or 1)
    endif
    ifeq ($(CUP_OFFICIAL_BUILD),1)
        ifneq ($(CONFIGURATION),release)
            $(error Official build identity is valid only for the release configuration)
        endif
        ifneq ($(strip $(EXTRA_CPPFLAGS) $(EXTRA_CFLAGS) $(EXTRA_LDFLAGS) $(EXTRA_LDLIBS)),)
            $(error Official builds do not accept EXTRA_* flags)
        endif
    endif
endif

# Configuration-specific outputs and generated sources.
BUILD_DIR := build
ifneq ($(findstring $(space),$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain whitespace; checkout paths containing spaces are supported)
endif
CONFIG_DIR := $(BUILD_DIR)/$(PLATFORM)/$(CONFIGURATION)
OBJ_DIR := $(CONFIG_DIR)/obj
BIN_DIR := $(CONFIG_DIR)/bin
GENERATED_DIR := $(CONFIG_DIR)/generated
BUILD_CONFIG := $(CONFIG_DIR)/build-config.txt
VERSION_STAMP := $(GENERATED_DIR)/.version-stamp
VERSION_HEADER := $(GENERATED_DIR)/version.h
VERSION_RESOURCE := $(GENERATED_DIR)/version.rc
VERSION_METADATA := $(GENERATED_DIR)/release.txt
CA_BUNDLE_STAMP := $(GENERATED_DIR)/.ca-bundle-stamp
CA_BUNDLE_HEADER := $(GENERATED_DIR)/ca_bundle.h
CA_BUNDLE_SOURCE := $(GENERATED_DIR)/ca_bundle.c
BINARY_INSPECTION := $(CONFIG_DIR)/binary-inspection.txt

# Portable production modules; one system implementation is selected below.
COMMON_SRC := \
    src/main.c \
    src/exit_status.c \
    src/wrappers.c \
    src/command_update.c \
    src/cup_update.c \
    src/package_selector.c \
    src/package_request.c \
    src/command_context.c \
    src/package_install.c \
    src/command_install.c \
    src/command_config.c \
    src/command_remove.c \
    src/command_list.c \
    src/command_default.c \
    src/command_info.c \
    src/command_search.c \
    src/command_inspect.c \
    src/command_doctor.c \
    src/command_repair.c \
    src/command_uninstall.c \
    src/state.c \
    src/filesystem.c \
    src/layout.c \
    src/package_catalog.c \
    src/install_policy.c \
    src/tool_preferences.c \
    src/package_metadata.c \
    src/checksum.c \
    src/sha256.c \
    src/cup_assets.c \
    src/package.c \
    src/installed_package.c \
    src/package_transaction.c \
    src/cup_update_journal.c \
    src/runtime_journal.c \
    src/cup_update_helper.c \
    src/text.c \
    src/registry.c \
    src/download.c \
    src/package_cache.c \
    src/package_archive_format.c \
    src/package_archive.c \
    src/package_extract.c \
    src/path.c \
    src/interrupt.c \
    src/platform.c

# Mandatory project, platform, configuration and dependency flags are kept
# separate until the final override. Only EXTRA_* values are user additions.
PROJECT_CPPFLAGS := -I$(call escape_spaces,$(GENERATED_DIR)) \
    -I$(call escape_spaces,$(PROJECT_ROOT)/include) \
    -DCUP_USE_EMBEDDED_CA_BUNDLE
PROJECT_CFLAGS := -Wall -Wextra -Werror -std=c11
PROJECT_LDFLAGS :=
PROJECT_LDLIBS :=
PLATFORM_CPPFLAGS :=
PLATFORM_CFLAGS :=
PLATFORM_LDFLAGS :=
PLATFORM_LDLIBS :=
DEPENDENCY_CPPFLAGS :=
DEPENDENCY_CFLAGS :=
DEPENDENCY_LDFLAGS :=
DEPENDENCY_LDLIBS :=

CONFIG_CFLAGS_development := -O0 -g3
CONFIG_CFLAGS_debug := -O0 -g3 -fno-omit-frame-pointer \
    -fno-optimize-sibling-calls
CONFIG_CFLAGS_coverage := -O0 -g3 --coverage -fprofile-arcs -ftest-coverage
CONFIG_LDFLAGS_coverage := --coverage
CONFIG_CFLAGS_sanitizers := -O0 -g3 -fsanitize=address,undefined \
    -fno-omit-frame-pointer
CONFIG_LDFLAGS_sanitizers := -fsanitize=address,undefined
CONFIG_CFLAGS_release := -O2 -DNDEBUG

# Native/cross toolchain selection by public platform identifier. Explicit
# command-line CC/WINDRES values remain available for compiler-matrix and MSYS2
# jobs because GNU Make command-line variables override these defaults.
ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64 macos-x64 macos-arm64),)
    SYSTEM_SRC := src/system_posix.c
    TARGET := $(BIN_DIR)/cup
endif

ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64),)
    CC := gcc
    PLATFORM_CPPFLAGS += -D_POSIX_C_SOURCE=200809L
endif

ifneq ($(filter $(PLATFORM),macos-x64 macos-arm64),)
    CC := clang
    ifneq ($(origin MACOSX_DEPLOYMENT_TARGET),undefined)
        ifneq ($(MACOSX_DEPLOYMENT_TARGET),$(CUP_MACOS_DEPLOYMENT_TARGET))
            $(error macOS builds require MACOSX_DEPLOYMENT_TARGET=$(CUP_MACOS_DEPLOYMENT_TARGET))
        endif
    endif
    override MACOSX_DEPLOYMENT_TARGET := $(CUP_MACOS_DEPLOYMENT_TARGET)
    export MACOSX_DEPLOYMENT_TARGET
    PLATFORM_CPPFLAGS += -D_DARWIN_C_SOURCE
    PLATFORM_CFLAGS += -mmacosx-version-min=$(CUP_MACOS_DEPLOYMENT_TARGET)
    PLATFORM_LDFLAGS += -mmacosx-version-min=$(CUP_MACOS_DEPLOYMENT_TARGET)
endif

ifeq ($(PLATFORM),windows-x64)
    CC := gcc
    WINDRES := windres
    SYSTEM_SRC := src/system_windows.c
    TARGET := $(BIN_DIR)/cup.exe
    RESOURCE_OBJ := $(OBJ_DIR)/version-resource.o
    PLATFORM_CPPFLAGS += -D_WIN32_WINNT=$(CUP_WINDOWS_WINNT) \
        -DWINVER=$(CUP_WINDOWS_WINNT)
endif

# Configuration-specific instrumentation and diagnostics.
ifeq ($(CONFIGURATION),coverage)
    ifeq ($(filter $(PLATFORM),linux-x64 linux-arm64),)
        $(error The coverage configuration is currently supported only on Linux)
    endif
endif

ifeq ($(CONFIGURATION),sanitizers)
    ifneq ($(PLATFORM),linux-x64)
        $(error The sanitizers configuration is currently supported only on linux-x64)
    endif
endif

ifeq ($(CONFIGURATION),debug)
    ifneq ($(filter macos-x64 macos-arm64,$(PLATFORM)),)
        CONFIG_CFLAGS_debug += -gdwarf-4 -fstandalone-debug -fno-limit-debug-info
    else ifneq ($(findstring clang,$(notdir $(CC))),)
        CONFIG_CFLAGS_debug += -gdwarf-4 -fstandalone-debug -fno-limit-debug-info
    else
        CONFIG_CFLAGS_debug += -gdwarf-5 -fvar-tracking \
            -fvar-tracking-assignments -grecord-gcc-switches
    endif
endif

# Every configuration uses one complete pinned third-party graph. Release adds
# the platform standalone policy, but it does not select a different set of
# headers or libraries from development, debug, coverage or sanitizers.
DEPS_PREFIX ?= $(HOME)/deps/$(PLATFORM)/install
DEPS_PREFIX_INPUT := $(DEPS_PREFIX)
ifneq ($(findstring $(space),$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain whitespace)
endif
override DEPS_PREFIX := $(abspath $(DEPS_PREFIX_INPUT))
DEPS_INCLUDE := $(DEPS_PREFIX)/include
DEPS_LIB_DIRS := $(DEPS_PREFIX)/lib $(DEPS_PREFIX)/lib64
ARGTABLE_LIB = $(firstword $(wildcard \
    $(DEPS_PREFIX)/lib/libargtable3.a \
    $(DEPS_PREFIX)/lib64/libargtable3.a \
    $(DEPS_PREFIX)/lib/libargtable3.dll.a \
    $(DEPS_PREFIX)/lib64/libargtable3.dll.a))
CURL_CONFIG := $(DEPS_PREFIX)/bin/curl-config
CURL_LIBS := $(shell $(CURL_CONFIG) --static-libs 2>/dev/null)
STATIC_PKG_CONFIG_PATH := \
    $(DEPS_PREFIX)/lib/pkgconfig:$(DEPS_PREFIX)/lib64/pkgconfig
ARCHIVE_LIBS := $(shell \
    PKG_CONFIG_PATH=$(STATIC_PKG_CONFIG_PATH) \
    PKG_CONFIG_LIBDIR=$(STATIC_PKG_CONFIG_PATH) \
    PKG_CONFIG_SYSROOT_DIR= \
    pkg-config --static --libs libarchive 2>/dev/null)

ifeq ($(NEED_BUILD_CONFIG),1)
    ifeq ($(wildcard $(CURL_CONFIG)),)
        $(error Missing $(CURL_CONFIG). Run 'make PLATFORM=$(PLATFORM) deps' first.)
    endif
    ifeq ($(strip $(CURL_LIBS)),)
        $(error curl-config did not return pinned static link flags)
    endif
    ifeq ($(strip $(ARCHIVE_LIBS)),)
        $(error pkg-config did not return pinned static libarchive link flags)
    endif
    ifeq ($(strip $(ARGTABLE_LIB)),)
        $(error Missing static Argtable3 archive in $(DEPS_PREFIX))
    endif
endif

DEPENDENCY_CPPFLAGS += -I$(DEPS_INCLUDE)
DEPENDENCY_LDFLAGS += $(addprefix -L,$(DEPS_LIB_DIRS))
DEPENDENCY_LDLIBS += $(ARGTABLE_LIB) $(CURL_LIBS) $(ARCHIVE_LIBS)

ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64 macos-x64 macos-arm64),)
    PLATFORM_CPPFLAGS += -DCUP_USE_OPENSSL_INIT
endif

ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64),)
    PLATFORM_LDLIBS += -ldl -pthread
endif

ifeq ($(PLATFORM),windows-x64)
    PLATFORM_CPPFLAGS += -DCURL_STATICLIB
    PLATFORM_LDLIBS += -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 \
        -liphlpapi -lsecur32
endif

ifeq ($(CONFIGURATION),release)
    ifneq ($(filter $(PLATFORM),linux-x64 linux-arm64 windows-x64),)
        PLATFORM_LDFLAGS += -static
    endif
endif

# These overrides deliberately ignore ambient/direct flag variables. The full
# resulting values, including local EXTRA_* additions, are persisted below.
override CPPFLAGS := $(strip $(PROJECT_CPPFLAGS) $(PLATFORM_CPPFLAGS) \
    $(CONFIG_CPPFLAGS_$(CONFIGURATION)) $(DEPENDENCY_CPPFLAGS) \
    $(EXTRA_CPPFLAGS))
override CFLAGS := $(strip $(PROJECT_CFLAGS) $(PLATFORM_CFLAGS) \
    $(CONFIG_CFLAGS_$(CONFIGURATION)) $(DEPENDENCY_CFLAGS) $(EXTRA_CFLAGS))
override LDFLAGS := $(strip $(PROJECT_LDFLAGS) $(PLATFORM_LDFLAGS) \
    $(CONFIG_LDFLAGS_$(CONFIGURATION)) $(DEPENDENCY_LDFLAGS) \
    $(EXTRA_LDFLAGS))
override LDLIBS := $(strip $(PROJECT_LDLIBS) $(PLATFORM_LDLIBS) \
    $(CONFIG_LDLIBS_$(CONFIGURATION)) $(DEPENDENCY_LDLIBS) \
    $(EXTRA_LDLIBS))

SRC := $(COMMON_SRC) $(SYSTEM_SRC)
CA_BUNDLE_OBJ := $(OBJ_DIR)/ca_bundle.o
OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC)) $(CA_BUNDLE_OBJ) $(RESOURCE_OBJ)
DEP := $(filter %.d,$(OBJ:.o=.d))
MDBOOK := $(if $(wildcard ./mdbook),./mdbook,mdbook)

# Build, maintenance, test and documentation entry points.
.PHONY: \
    all debug coverage sanitizers release help _build clean reset-dev-home \
    deps check-dependencies check-toolchain check-binary docs-assets docs serve version \
    release-metadata \
    validate-release test test-posix test-integration test-unit \
    test-unit-build test-helpers test-build test-repository test-release \
    test-windows test-portability-linux test-coverage test-sanitizers \
    update-ca-bundle FORCE

BUILD_RECURSE = $(MAKE) --no-print-directory _build \
    PLATFORM='$(PLATFORM)' DEPS_PREFIX='$(DEPS_PREFIX)' \
    CC='$(CC)' WINDRES='$(WINDRES)' \
    CUP_OFFICIAL_BUILD='$(CUP_OFFICIAL_BUILD)'

all:
	+@$(BUILD_RECURSE) CUP_BUILD_CONFIGURATION=development

debug:
	+@$(BUILD_RECURSE) CUP_BUILD_CONFIGURATION=debug

coverage:
	+@$(BUILD_RECURSE) CUP_BUILD_CONFIGURATION=coverage

sanitizers:
	+@$(BUILD_RECURSE) CUP_BUILD_CONFIGURATION=sanitizers

release:
	+@$(BUILD_RECURSE) CUP_BUILD_CONFIGURATION=release

help:
	@printf '%s\n' \
		'Build targets:' \
		'  make              development build' \
		'  make debug        diagnostic build with rich symbols' \
		'  make coverage     coverage-instrumented build' \
		'  make sanitizers   ASan/UBSan build (linux-x64)' \
		'  make release      standalone release-mode build' \
		'  make check-binary verify the current native executable policy' \
		'  JOBS=4 make deps  prepare pinned dependencies with controlled parallelism' \
		'  make test         native regression suite' \
		'  make test-portability-linux  Linux HTTPS/proxy/package smoke test' \
		'  make clean        remove build outputs' \
		'' \
		'Local flag additions: EXTRA_CPPFLAGS, EXTRA_CFLAGS, EXTRA_LDFLAGS, EXTRA_LDLIBS' \
		'Platform selector: PLATFORM=$(SUPPORTED_PLATFORM)'

_build: $(TARGET)

deps:
	@case "$(PLATFORM)" in \
		linux-*|macos-*) builder=./scripts/dependencies/build-posix.sh ;; \
		windows-x64) builder=./scripts/dependencies/build-windows.sh ;; \
		*) \
			echo "Unsupported PLATFORM '$(PLATFORM)' for dependencies." >&2; \
			exit 1 ;; \
	esac; \
	JOBS='$(JOBS)' PLATFORM='$(PLATFORM)' DEPS_PREFIX='$(DEPS_PREFIX)' \
		MACOSX_DEPLOYMENT_TARGET='$(MACOSX_DEPLOYMENT_TARGET)' \
		bash "$$builder"

check-dependencies:
	@./scripts/dependencies/verify.sh \
		'$(PLATFORM)' '$(DEPS_PREFIX)' || { \
		echo "Run 'make PLATFORM=$(PLATFORM) deps' first." >&2; \
		exit 1; \
	}

check-toolchain:
	@./scripts/build/validate-toolchain.sh \
		'$(PLATFORM)' '$(CC)' '$(WINDRES)'

check-binary: $(BINARY_INSPECTION)
	@cat "$(BINARY_INSPECTION)"

$(BINARY_INSPECTION): $(TARGET) scripts/build/inspect-binary.sh
	@./scripts/build/inspect-binary.sh \
		'$(PLATFORM)' '$(CONFIGURATION)' '$(TARGET)' '$@'

FORCE:

$(VERSION_STAMP): FORCE VERSION scripts/version.sh
	@mkdir -p "$(GENERATED_DIR)"
	@CUP_OFFICIAL_BUILD='$(VERSION_OFFICIAL_BUILD)' \
		CUP_BUILD_CONFIGURATION='$(CONFIGURATION)' \
		./scripts/version.sh generate "$(GENERATED_DIR)"
	@touch "$@"

$(VERSION_HEADER) $(VERSION_RESOURCE) $(VERSION_METADATA): $(VERSION_STAMP)
	@test -f "$@" || { \
		rm -f -- "$(VERSION_STAMP)"; \
		$(MAKE) "$(VERSION_STAMP)"; \
	}

$(CA_BUNDLE_STAMP): certs/cacert.pem scripts/certs/generate-ca-bundle.sh
	@mkdir -p "$(GENERATED_DIR)"
	@./scripts/certs/generate-ca-bundle.sh certs/cacert.pem "$(GENERATED_DIR)"
	@touch "$@"

$(CA_BUNDLE_HEADER) $(CA_BUNDLE_SOURCE): $(CA_BUNDLE_STAMP)
	@test -f "$@" || { \
		rm -f -- "$(CA_BUNDLE_STAMP)"; \
		$(MAKE) "$(CA_BUNDLE_STAMP)"; \
	}

$(BUILD_CONFIG): FORCE Makefile scripts/build/write-config.sh | check-dependencies check-toolchain
	@CUP_BUILD_PLATFORM='$(PLATFORM)' \
		CUP_BUILD_CONFIGURATION='$(CONFIGURATION)' \
		CUP_BUILD_CC='$(CC)' \
		CUP_BUILD_WINDRES='$(WINDRES)' \
		CUP_BUILD_CPPFLAGS='$(CPPFLAGS)' \
		CUP_BUILD_CFLAGS='$(CFLAGS)' \
		CUP_BUILD_LDFLAGS='$(LDFLAGS)' \
		CUP_BUILD_LDLIBS='$(LDLIBS)' \
		CUP_BUILD_DEPS_PREFIX='$(DEPS_PREFIX)' \
		CUP_BUILD_OFFICIAL='$(CUP_OFFICIAL_BUILD)' \
		./scripts/build/write-config.sh '$@'

$(OBJ): $(BUILD_CONFIG) | $(VERSION_STAMP) $(CA_BUNDLE_STAMP)

$(TARGET): $(OBJ) $(BUILD_CONFIG)
	@mkdir -p "$(BIN_DIR)"
	$(CC) $(CFLAGS) $(CPPFLAGS) $(OBJ) -o "$@" $(LDFLAGS) $(LDLIBS)

$(CA_BUNDLE_OBJ): $(CA_BUNDLE_SOURCE) $(CA_BUNDLE_HEADER) $(BUILD_CONFIG)
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c "$(CA_BUNDLE_SOURCE)" -o "$@"

$(OBJ_DIR)/%.o: src/%.c $(BUILD_CONFIG)
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c "$<" -o "$@"

ifeq ($(PLATFORM),windows-x64)
$(RESOURCE_OBJ): $(VERSION_RESOURCE) $(BUILD_CONFIG)
	@mkdir -p "$(dir $@)"
	$(WINDRES) -I$(call escape_spaces,$(GENERATED_DIR)) "$<" -O coff -o "$@"
endif

-include $(DEP)

version: $(VERSION_STAMP)
	@cat "$(VERSION_METADATA)"

validate-release:
	@CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release \
		./scripts/version.sh validate-release

release-metadata: $(VERSION_STAMP)
	@printf '%s\n' '$(VERSION_METADATA)'

clean:
	@rm -rf -- "$(BUILD_DIR)"

reset-dev-home:
	@test "$(CUP_ALLOW_DEV_CLEAN)" = "1" || { \
		echo "Refusing to remove $(HOME)/.cup without CUP_ALLOW_DEV_CLEAN=1" >&2; \
		exit 1; \
	}
	@case "$(HOME)" in \
		/*) test "$(HOME)" != "/" ;; \
		*) false ;; \
	esac || { echo "Invalid HOME for reset-dev-home" >&2; exit 1; }
	@rm -rf -- "$(BUILD_DIR)"
	@rm -rf -- "$(HOME)/.cup"
	@clear 2>/dev/null || true

# The Makefile owns compilation of product and test binaries. Runners only
# execute already-built programs and compose behavioral suites.
CUP_TEST_CONFIGURATION ?= development

test-unit-build:
	@CUP_TEST_PLATFORM='$(PLATFORM)' \
		CUP_TEST_CONFIGURATION='$(CUP_TEST_CONFIGURATION)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		./tests/build/unit.sh

test-helpers:
	@CUP_TEST_PLATFORM='$(PLATFORM)' \
		CUP_TEST_CONFIGURATION='$(CUP_TEST_CONFIGURATION)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		./tests/build/helpers.sh

test-build:
	+@$(MAKE) --no-print-directory all PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)'
	+@$(MAKE) --no-print-directory test-unit-build test-helpers \
		PLATFORM='$(PLATFORM)' DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		CUP_TEST_CONFIGURATION=development

test:
	@case '$(PLATFORM)' in \
		windows-x64) \
			$(MAKE) --no-print-directory test-windows PLATFORM='$(PLATFORM)' \
				DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' ;; \
		*) \
			$(MAKE) --no-print-directory test-build PLATFORM='$(PLATFORM)' \
				DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' && \
			CUP_TEST_SKIP_BUILD=1 CUP_TEST_CONFIGURATION=development \
				./tests/runners/all-posix.sh ;; \
	esac

test-unit:
	+@$(MAKE) --no-print-directory test-unit-build PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		CUP_TEST_CONFIGURATION=development
	@CUP_TEST_CONFIGURATION=development ./tests/runners/unit.sh

test-repository:
	@./tests/runners/repository.sh

test-posix: test-integration

test-integration:
	+@$(MAKE) --no-print-directory all test-helpers PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		CUP_TEST_CONFIGURATION=development
	@CUP_TEST_SKIP_BUILD=1 CUP_TEST_CONFIGURATION=development \
		./tests/runners/integration-posix.sh

test-release:
	@test -n "$(RELEASE_DIR)" || { echo "Set RELEASE_DIR=<candidate-dir>" >&2; exit 2; }
	+@$(MAKE) --no-print-directory test-helpers PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		CUP_TEST_CONFIGURATION=development
	@CUP_TEST_CONFIGURATION=development ./tests/release/posix.sh "$(RELEASE_DIR)"

test-coverage:
	@./tests/runners/coverage.sh

test-sanitizers:
	@./tests/runners/sanitizers.sh

test-portability-linux:
	@PLATFORM='$(PLATFORM)' DEPS_PREFIX='$(DEPS_PREFIX)' \
		./tests/portability/linux-network.sh

test-windows:
	+@$(MAKE) --no-print-directory test-build PLATFORM=windows-x64 \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)'
	@CUP_TEST_PLATFORM=windows-x64 CUP_TEST_CONFIGURATION=development \
		./tests/runners/unit.sh
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
		"$$(cygpath -w '$(PROJECT_ROOT)/tests/integration/windows/run.ps1')" \
		-CupPath "$$(cygpath -w '$(PROJECT_ROOT)/build/windows-x64/development/bin/cup.exe')"
	@./tests/runners/repository.sh

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
