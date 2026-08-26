# Build selectors and supported configurations.
.DEFAULT_GOAL := all
override PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# GNU Make treats whitespace as a list separator. The checkout itself may live
# in a path containing ordinary spaces, so paths embedded in compiler flags are
# escaped explicitly. User-selected build and dependency roots remain
# whitespace-free until all supported metadata formats can quote them portably.
empty :=
space := $(empty) $(empty)
apostrophe := '
colon := :
semicolon := ;
percent := %
hash := \#
dollar := $$
left_paren := (
left_brace := {
escape_spaces = $(subst $(space),\$(space),$(1))

SUPPORTED_PLATFORM := linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64
SUPPORTED_CONFIGURATION := development debug coverage sanitizers release
PLATFORM_INPUT_ORIGIN := $(origin PLATFORM)

# Freeze every supported external input before it is inspected. $(value ...)
# preserves command-line and environment text literally, so Make functions in
# inherited values cannot execute while this Makefile is parsed.
ifneq ($(filter environment environment\ override command\ line,$(origin PLATFORM)),)
override PLATFORM := $(value PLATFORM)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin OS)),)
override OS := $(value OS)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin PROCESSOR_ARCHITEW6432)),)
override PROCESSOR_ARCHITEW6432 := $(value PROCESSOR_ARCHITEW6432)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin PROCESSOR_ARCHITECTURE)),)
override PROCESSOR_ARCHITECTURE := $(value PROCESSOR_ARCHITECTURE)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin HOME)),)
override HOME := $(value HOME)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_BUILD_CONFIGURATION)),)
override CUP_BUILD_CONFIGURATION := $(value CUP_BUILD_CONFIGURATION)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_OFFICIAL_BUILD)),)
override CUP_OFFICIAL_BUILD := $(value CUP_OFFICIAL_BUILD)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_INTERNAL_DEPS_TARGET)),)
override CUP_INTERNAL_DEPS_TARGET := $(value CUP_INTERNAL_DEPS_TARGET)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_INTERNAL_TOOLCHAIN_ROLE)),)
override CUP_INTERNAL_TOOLCHAIN_ROLE := $(value CUP_INTERNAL_TOOLCHAIN_ROLE)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin BUILD_DIR)),)
override BUILD_DIR := $(value BUILD_DIR)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin DEPS_ROOT)),)
override DEPS_ROOT := $(value DEPS_ROOT)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin DEPS_PREFIX)),)
override DEPS_PREFIX := $(value DEPS_PREFIX)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CC)),)
override CC := $(value CC)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin WINDRES)),)
override WINDRES := $(value WINDRES)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin JOBS)),)
override JOBS := $(value JOBS)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin MACOSX_DEPLOYMENT_TARGET)),)
override MACOSX_DEPLOYMENT_TARGET := $(value MACOSX_DEPLOYMENT_TARGET)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_TEST_CONFIGURATION)),)
override CUP_TEST_CONFIGURATION := $(value CUP_TEST_CONFIGURATION)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin RELEASE_DIR)),)
override RELEASE_DIR := $(value RELEASE_DIR)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin MDBOOK)),)
override MDBOOK := $(value MDBOOK)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin EXTRA_CPPFLAGS)),)
override EXTRA_CPPFLAGS := $(value EXTRA_CPPFLAGS)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin EXTRA_CFLAGS)),)
override EXTRA_CFLAGS := $(value EXTRA_CFLAGS)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin EXTRA_LDFLAGS)),)
override EXTRA_LDFLAGS := $(value EXTRA_LDFLAGS)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin EXTRA_LDLIBS)),)
override EXTRA_LDLIBS := $(value EXTRA_LDLIBS)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_RELEASE_VERSION)),)
override CUP_RELEASE_VERSION := $(value CUP_RELEASE_VERSION)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_RELEASE_TAG)),)
override CUP_RELEASE_TAG := $(value CUP_RELEASE_TAG)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_RELEASE_COMMIT)),)
override CUP_RELEASE_COMMIT := $(value CUP_RELEASE_COMMIT)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin SOURCE_REPOSITORY)),)
override SOURCE_REPOSITORY := $(value SOURCE_REPOSITORY)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin TESTS_RUN_ID)),)
override TESTS_RUN_ID := $(value TESTS_RUN_ID)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin TESTS_RUN_ATTEMPT)),)
override TESTS_RUN_ATTEMPT := $(value TESTS_RUN_ATTEMPT)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin TESTS_EVIDENCE_INDEX_SHA256)),)
override TESTS_EVIDENCE_INDEX_SHA256 := $(value TESTS_EVIDENCE_INDEX_SHA256)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin RELEASE_RUN_ID)),)
override RELEASE_RUN_ID := $(value RELEASE_RUN_ID)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin RELEASE_RUN_ATTEMPT)),)
override RELEASE_RUN_ATTEMPT := $(value RELEASE_RUN_ATTEMPT)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin MSYSTEM)),)
override MSYSTEM := $(value MSYSTEM)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_DEPENDENCY_PROFILE)),)
override CUP_DEPENDENCY_PROFILE := $(value CUP_DEPENDENCY_PROFILE)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin RELEASE_COMMON_ROOT)),)
override RELEASE_COMMON_ROOT := $(value RELEASE_COMMON_ROOT)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin RELEASE_COMMON_DIR)),)
override RELEASE_COMMON_DIR := $(value RELEASE_COMMON_DIR)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin RELEASE_PLATFORM_ROOT)),)
override RELEASE_PLATFORM_ROOT := $(value RELEASE_PLATFORM_ROOT)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_ALLOW_DEV_CLEAN)),)
override CUP_ALLOW_DEV_CLEAN := $(value CUP_ALLOW_DEV_CLEAN)
endif
ifneq ($(filter environment environment\ override command\ line,$(origin CUP_TEST_WITH_BUILD_OUTPUT)),)
override CUP_TEST_WITH_BUILD_OUTPUT := $(value CUP_TEST_WITH_BUILD_OUTPUT)
endif

# Reject nested Make expressions before any frozen value is passed to another
# Make function. $(value ...) is the only expansion that exposes caller text
# without evaluating it. Explicit EXTRA_* flag strings are trusted compiler
# additions and remain outside this check.
override RAW_MAKE_INPUTS := \
    PLATFORM OS PROCESSOR_ARCHITEW6432 PROCESSOR_ARCHITECTURE HOME \
    CUP_BUILD_CONFIGURATION CUP_OFFICIAL_BUILD CUP_INTERNAL_DEPS_TARGET \
    CUP_INTERNAL_TOOLCHAIN_ROLE BUILD_DIR DEPS_ROOT DEPS_PREFIX CC WINDRES JOBS \
    MACOSX_DEPLOYMENT_TARGET \
    CUP_TEST_CONFIGURATION RELEASE_DIR MDBOOK EXTRA_CPPFLAGS EXTRA_CFLAGS \
    EXTRA_LDFLAGS EXTRA_LDLIBS CUP_RELEASE_VERSION \
    CUP_RELEASE_TAG CUP_RELEASE_COMMIT SOURCE_REPOSITORY TESTS_RUN_ID \
    TESTS_RUN_ATTEMPT TESTS_EVIDENCE_INDEX_SHA256 RELEASE_RUN_ID \
    RELEASE_RUN_ATTEMPT MSYSTEM CUP_DEPENDENCY_PROFILE RELEASE_COMMON_ROOT \
    RELEASE_COMMON_DIR RELEASE_PLATFORM_ROOT CUP_ALLOW_DEV_CLEAN \
    CUP_TEST_WITH_BUILD_OUTPUT
override MAKE_PAREN_OPEN := $(dollar)$(left_paren)
override MAKE_BRACE_OPEN := $(dollar)$(left_brace)
ifneq ($(strip $(foreach variable,$(RAW_MAKE_INPUTS),\
        $(if $(findstring $(MAKE_PAREN_OPEN),$(value $(variable))),$(variable))\
        $(if $(findstring $(MAKE_BRACE_OPEN),$(value $(variable))),$(variable)))),)
    $(error Caller-controlled Make expressions are not supported)
endif
tab := $(shell printf '\t')

# Derived paths, file lists and policy variables are private implementation
# details. Refuse command-line replacement instead of silently building or
# cleaning a caller-selected path under an internal variable name.
override PRIVATE_MAKE_VARIABLES := \
    HOST_SYSTEM HOST_MACHINE WINDOWS_MACHINE NATIVE_PLATFORM \
    VERSION_OFFICIAL_BUILD NEED_BUILD_CONFIG BUILD_ROOT CONFIG_DIR OBJ_DIR \
    BIN_DIR GENERATED_DIR BUILD_CONFIG VERSION_STAMP VERSION_HEADER \
    VERSION_RESOURCE VERSION_METADATA CA_BUNDLE_STAMP CA_BUNDLE_HEADER \
    CA_BUNDLE_SOURCE BINARY_INSPECTION TARGET OBJ DEP SRC SYSTEM_SRC \
    COMMON_SRC RESOURCE_OBJ COVERAGE_ENTRY_OBJ DEPS_INCLUDE DEPS_LIB_DIRS \
    CURL_CONFIG PKG_CONFIG_PATH BUILD_ROOT_MARKER FINALIZED_ROOT
ifneq ($(strip $(foreach variable,$(PRIVATE_MAKE_VARIABLES),\
        $(if $(filter command\ line,$(origin $(variable))),$(variable)))),)
    $(error Private Make variables cannot be overridden from the command line)
endif

ifneq ($(findstring $(apostrophe),$(PROJECT_ROOT)),)
    $(error The checkout path must not contain a single quote)
endif

# Current CI build targets are explicit so cup and its pinned dependency graph
# agree. They are provisional compatibility baselines until native minimum-OS
# evidence is collected; they are not an unconditional long-term support claim.
CUP_MACOS_DEPLOYMENT_TARGET := 13.0
CUP_WINDOWS_WINNT := 0x0A00

override HOST_SYSTEM := $(shell uname -s 2>/dev/null)
override HOST_MACHINE := $(shell uname -m 2>/dev/null)
override WINDOWS_MACHINE := $(strip \
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
ifeq ($(PLATFORM_INPUT_ORIGIN),environment)
    ifeq ($(filter $(PLATFORM),$(SUPPORTED_PLATFORM)),)
        override PLATFORM := $(NATIVE_PLATFORM)
    endif
endif
PLATFORM ?= $(NATIVE_PLATFORM)

# Mandatory project flags are owned by the Makefile. Command-line additions use
# the explicit EXTRA_* variables so language, warning and linkage policy cannot
# be erased accidentally. Environment values are ignored by the final override.
DIRECT_FLAG_VARIABLES := CPPFLAGS CFLAGS LDFLAGS LDLIBS
ifneq ($(strip $(foreach variable,$(DIRECT_FLAG_VARIABLES),\
        $(if $(filter command\ line,$(origin $(variable))),$(variable)))),)
    $(error Direct CPPFLAGS/CFLAGS/LDFLAGS/LDLIBS overrides are not supported; use \
        EXTRA_CPPFLAGS, EXTRA_CFLAGS, EXTRA_LDFLAGS or EXTRA_LDLIBS)
endif

# CUP_BUILD_CONFIGURATION, CUP_OFFICIAL_BUILD, CUP_INTERNAL_DEPS_TARGET and
# CUP_INTERNAL_TOOLCHAIN_ROLE are internal recursive-make inputs used by public
# targets and CI/release consumers. They are not public user-facing selectors.
ifeq ($(origin CONFIGURATION),command line)
    $(error CONFIGURATION is internal; select make, debug, coverage, sanitizers or release)
endif
CUP_BUILD_CONFIGURATION ?= development
CUP_OFFICIAL_BUILD ?= 0
CUP_INTERNAL_DEPS_TARGET ?= deps
CUP_INTERNAL_TOOLCHAIN_ROLE ?= primary
override CONFIGURATION := $(CUP_BUILD_CONFIGURATION)

ifneq ($(filter $(CUP_INTERNAL_DEPS_TARGET),deps deps-check),$(CUP_INTERNAL_DEPS_TARGET))
    $(error CUP_INTERNAL_DEPS_TARGET must be deps or deps-check)
endif
ifneq ($(filter $(CUP_INTERNAL_TOOLCHAIN_ROLE),primary secondary),$(CUP_INTERNAL_TOOLCHAIN_ROLE))
    $(error CUP_INTERNAL_TOOLCHAIN_ROLE must be primary or secondary)
endif

VERSION_OFFICIAL_BUILD := 0
ifeq ($(CONFIGURATION),release)
    VERSION_OFFICIAL_BUILD := $(CUP_OFFICIAL_BUILD)
endif

# Public targets enter a recursive private build target. Selector validation is
# therefore attached to the private build boundary rather than maintained as a
# fragile negative list of every non-build goal.
BUILD_ENTRY_GOALS := _build _check-binary _debug-artifact _release-candidate
ifneq ($(strip $(filter $(BUILD_ENTRY_GOALS),$(MAKECMDGOALS))),)
    NEED_BUILD_CONFIG := 1
else
    NEED_BUILD_CONFIG := 0
endif

ifeq ($(NEED_BUILD_CONFIG),1)
    ifeq ($(filter $(PLATFORM),$(SUPPORTED_PLATFORM)),)
        $(error Unsupported PLATFORM '$(PLATFORM)'. Supported values: $(SUPPORTED_PLATFORM))
    endif
    ifeq ($(filter $(CONFIGURATION),$(SUPPORTED_CONFIGURATION)),)
        $(error Unsupported build configuration '$(CONFIGURATION)'. Supported values: $(SUPPORTED_CONFIGURATION))
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
BUILD_DIR ?= build
PATH_SAFETY := $(PROJECT_ROOT)/scripts/lib/path-safety.sh
ifneq ($(findstring $(space),$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain whitespace; checkout paths containing spaces are supported)
endif
ifneq ($(findstring $(tab),$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain whitespace)
endif
ifneq ($(findstring $(apostrophe),$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain a single quote)
endif
ifneq ($(findstring $(colon),$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain a colon)
endif
ifneq ($(findstring $(semicolon),$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain a semicolon)
endif
ifneq ($(findstring $(percent),$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain a percent sign)
endif
ifneq ($(findstring $(hash),$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain a number sign)
endif
ifneq ($(findstring $(dollar),$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain a dollar sign)
endif
ifneq ($(findstring \,$(BUILD_DIR)),)
    $(error BUILD_DIR must use forward slashes)
endif
ifneq ($(findstring //,$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain empty path components)
endif
ifneq ($(filter / . .. ./% ../% %/. %/.. %/,$(BUILD_DIR)),)
    $(error BUILD_DIR must be a clean relative or absolute path without . or .. components)
endif
ifneq ($(findstring /./,$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain . path components)
endif
ifneq ($(findstring /../,$(BUILD_DIR)),)
    $(error BUILD_DIR must not contain .. path components)
endif
override BUILD_ROOT := $(abspath $(BUILD_DIR))
override CONFIG_DIR := $(BUILD_DIR)/$(PLATFORM)/$(CONFIGURATION)
override OBJ_DIR := $(CONFIG_DIR)/obj
override BIN_DIR := $(CONFIG_DIR)/bin
override GENERATED_DIR := $(CONFIG_DIR)/generated
override BUILD_CONFIG := $(CONFIG_DIR)/build-config.txt
override VERSION_STAMP := $(GENERATED_DIR)/.version-stamp
override VERSION_HEADER := $(GENERATED_DIR)/version.h
override VERSION_RESOURCE := $(GENERATED_DIR)/version.rc
override VERSION_METADATA := $(GENERATED_DIR)/release.txt
override CA_BUNDLE_STAMP := $(GENERATED_DIR)/.ca-bundle-stamp
override CA_BUNDLE_HEADER := $(GENERATED_DIR)/ca_bundle.h
override CA_BUNDLE_SOURCE := $(GENERATED_DIR)/ca_bundle.c
override BINARY_INSPECTION := $(CONFIG_DIR)/binary-inspection.txt

# Portable production modules; one system implementation is selected below.
COMMON_SRC := \
    src/main.c \
    src/exit_status.c \
    src/wrappers.c \
    src/command_update.c \
    src/self_update.c \
    src/bootstrap.c \
    src/release_metadata.c \
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
    src/third_party/sha256.c \
    src/assets.c \
    src/package.c \
    src/installed_package.c \
    src/package_transaction.c \
    src/update_journal.c \
    src/runtime_journal.c \
    src/uninstall_journal.c \
    src/uninstall_helper.c \
    src/update_helper.c \
    src/text.c \
    src/system.c \
    src/registry.c \
    src/download.c \
    src/download_url.c \
    src/package_cache.c \
    src/package_artifact.c \
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
PROJECT_CFLAGS := -Wall -Wextra -Werror -std=c11 \
    -ffile-prefix-map=$(call escape_spaces,$(BUILD_ROOT))=/usr/src/cup-build \
    -fdebug-prefix-map=$(call escape_spaces,$(BUILD_ROOT))=/usr/src/cup-build \
    -fmacro-prefix-map=$(call escape_spaces,$(BUILD_ROOT))=/usr/src/cup-build \
    -ffile-prefix-map=$(call escape_spaces,$(PROJECT_ROOT))=/usr/src/cup \
    -fdebug-prefix-map=$(call escape_spaces,$(PROJECT_ROOT))=/usr/src/cup \
    -fmacro-prefix-map=$(call escape_spaces,$(PROJECT_ROOT))=/usr/src/cup
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
CONFIG_CFLAGS_coverage := -O0 -g3 --coverage -fprofile-abs-path
CONFIG_LDFLAGS_coverage := --coverage
CONFIG_CFLAGS_sanitizers := -O0 -g3 -fsanitize=address,undefined \
    -fno-omit-frame-pointer
CONFIG_LDFLAGS_sanitizers := -fsanitize=address,undefined
CONFIG_CFLAGS_release := -O2 -g1 -DNDEBUG
CONFIG_CPPFLAGS_coverage :=
COVERAGE_ENTRY_OBJ :=

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
    # Apple ld already ignores repeated static-library flags. Preserve their
    # dependency-defined order and silence only the corresponding diagnostic.
    PLATFORM_LDFLAGS += -mmacosx-version-min=$(CUP_MACOS_DEPLOYMENT_TARGET) \
        -Wl,-no_warn_duplicate_libraries
    CONFIG_CFLAGS_coverage := -O0 -g3 -fprofile-instr-generate -fcoverage-mapping \
        -fcoverage-prefix-map=$(call escape_spaces,$(PROJECT_ROOT))=$(call escape_spaces,$(PROJECT_ROOT))
    CONFIG_LDFLAGS_coverage := -fprofile-instr-generate -fcoverage-mapping
    ifeq ($(CONFIGURATION),coverage)
        # Apple LLVM merges profiles from every instrumented executable. Give
        # them the same external wrapper and a unique real entry so unrelated
        # programs do not contribute incompatible main definitions.
        CONFIG_CPPFLAGS_coverage += \
            -DCUP_COVERAGE_ENTRY=cup_coverage_program_main
        COVERAGE_ENTRY_OBJ := $(OBJ_DIR)/coverage-entry.o
    endif
endif

ifeq ($(PLATFORM),windows-x64)
    CC := gcc
    ifneq ($(findstring clang,$(notdir $(CC))),)
        WINDRES := llvm-windres
    else
        WINDRES := windres
    endif
    SYSTEM_SRC := src/system_windows.c
    TARGET := $(BIN_DIR)/cup.exe
    RESOURCE_OBJ := $(OBJ_DIR)/version-resource.o
    PLATFORM_CPPFLAGS += -D_WIN32_WINNT=$(CUP_WINDOWS_WINNT) \
        -DWINVER=$(CUP_WINDOWS_WINNT)
endif

# Configuration-specific instrumentation and diagnostics.
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
DEPS_VARIANT := $(PLATFORM)
ifeq ($(PLATFORM),windows-x64)
    ifeq ($(MSYSTEM),CLANG64)
        DEPS_VARIANT := windows-x64-clang64
    endif
endif
DEPS_ROOT ?= $(HOME)/deps/$(DEPS_VARIANT)
DEPS_ROOT_INPUT := $(DEPS_ROOT)
ifneq ($(findstring $(space),$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain whitespace)
endif
ifneq ($(findstring $(tab),$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain whitespace)
endif
ifneq ($(findstring $(apostrophe),$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain a single quote)
endif
ifneq ($(findstring $(colon),$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain a colon)
endif
ifneq ($(findstring $(semicolon),$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain a semicolon)
endif
ifneq ($(findstring $(percent),$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain a percent sign)
endif
ifneq ($(findstring $(hash),$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain a number sign)
endif
ifneq ($(findstring $(dollar),$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain a dollar sign)
endif
ifneq ($(findstring \,$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must use forward slashes)
endif
ifneq ($(findstring //,$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain empty path components)
endif
ifeq ($(filter /%,$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must be an absolute path)
endif
ifneq ($(filter / . .. ./% ../% %/. %/.. %/,$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain . or .. path components)
endif
ifneq ($(findstring /./,$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain . path components)
endif
ifneq ($(findstring /../,$(DEPS_ROOT_INPUT)),)
    $(error DEPS_ROOT must not contain .. path components)
endif
override DEPS_ROOT := $(abspath $(DEPS_ROOT_INPUT))
DEPS_PREFIX ?= $(DEPS_ROOT)/install
DEPS_PREFIX_INPUT := $(DEPS_PREFIX)
ifneq ($(findstring $(space),$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain whitespace)
endif
ifneq ($(findstring $(tab),$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain whitespace)
endif
ifneq ($(findstring $(apostrophe),$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain a single quote)
endif
ifneq ($(findstring $(colon),$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain a colon)
endif
ifneq ($(findstring $(semicolon),$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain a semicolon)
endif
ifneq ($(findstring $(percent),$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain a percent sign)
endif
ifneq ($(findstring $(hash),$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain a number sign)
endif
ifneq ($(findstring $(dollar),$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain a dollar sign)
endif
ifneq ($(findstring \,$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must use forward slashes)
endif
ifneq ($(findstring //,$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain empty path components)
endif
ifeq ($(filter /%,$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must be an absolute path)
endif
ifneq ($(filter / . .. ./% ../% %/. %/.. %/,$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain . or .. path components)
endif
ifneq ($(findstring /./,$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain . path components)
endif
ifneq ($(findstring /../,$(DEPS_PREFIX_INPUT)),)
    $(error DEPS_PREFIX must not contain .. path components)
endif
override DEPS_PREFIX := $(abspath $(DEPS_PREFIX_INPUT))
override DEPS_INCLUDE := $(DEPS_PREFIX)/include
DEPS_LIB_DIRS = $(foreach directory,$(DEPS_PREFIX)/lib $(DEPS_PREFIX)/lib64,\
    $(if $(wildcard $(directory)),$(directory)))
ARGTABLE_LIB = $(firstword $(wildcard \
    $(DEPS_PREFIX)/lib/libargtable3.a \
    $(DEPS_PREFIX)/lib64/libargtable3.a \
    $(DEPS_PREFIX)/lib/libargtable3.dll.a \
    $(DEPS_PREFIX)/lib64/libargtable3.dll.a))
override CURL_CONFIG := $(DEPS_PREFIX)/bin/curl-config
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

# Test executables reuse the canonical platform and configuration flags, but
# select their configuration through CUP_TEST_CONFIGURATION. Apple coverage
# suites provide a unique entry-point macro for each executable below.
TEST_CPPFLAGS := $(filter-out -DCUP_COVERAGE_ENTRY=%,\
    -DCUP_USE_EMBEDDED_CA_BUNDLE $(PLATFORM_CPPFLAGS) \
    $(CONFIG_CPPFLAGS_$(CUP_TEST_CONFIGURATION)))
TEST_CFLAGS := $(PROJECT_CFLAGS) $(PLATFORM_CFLAGS) \
    $(CONFIG_CFLAGS_$(CUP_TEST_CONFIGURATION)) $(DEPENDENCY_CFLAGS)
TEST_LDFLAGS := $(PROJECT_LDFLAGS) $(PLATFORM_LDFLAGS) \
    $(CONFIG_LDFLAGS_$(CUP_TEST_CONFIGURATION)) $(DEPENDENCY_LDFLAGS)

SRC := $(COMMON_SRC) $(SYSTEM_SRC)
CA_BUNDLE_OBJ := $(OBJ_DIR)/ca_bundle.o
OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC)) $(CA_BUNDLE_OBJ) \
    $(COVERAGE_ENTRY_OBJ) $(RESOURCE_OBJ)
override PROJECT_HEADERS := $(wildcard include/*.h include/third_party/*.h)
override PROJECT_HEADER_DIRS := include include/third_party
MDBOOK := $(if $(wildcard ./mdbook),./mdbook,mdbook)

# Build, maintenance, test and documentation entry points.
override BUILD_ROOT_MARKER := $(BUILD_ROOT)/.cup-build-root
override FINALIZED_ROOT := $(BUILD_ROOT)/finalized/$(PLATFORM)/$(CONFIGURATION)
RELEASE_COMMON_ROOT ?= $(BUILD_DIR)/release/common
RELEASE_COMMON_DIR ?= $(RELEASE_COMMON_ROOT)/public
RELEASE_PLATFORM_ROOT ?= $(BUILD_DIR)/release/platforms/$(PLATFORM)

# Release paths are interpolated into shell recipes. Keep the same literal path
# domain as BUILD_DIR before converting relative values to absolute paths.
override RELEASE_PATH_VARIABLES := RELEASE_COMMON_ROOT RELEASE_COMMON_DIR RELEASE_PLATFORM_ROOT
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),$(if $(findstring $(space),$($(variable))),$(variable)))),)
    $(error Release paths must not contain whitespace)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),$(if $(findstring $(tab),$($(variable))),$(variable)))),)
    $(error Release paths must not contain whitespace)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),\
    $(if $(findstring $(apostrophe),$($(variable))),$(variable)))),)
    $(error Release paths must not contain a single quote)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),$(if $(findstring $(colon),$($(variable))),$(variable)))),)
    $(error Release paths must not contain a colon)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),\
    $(if $(findstring $(semicolon),$($(variable))),$(variable)))),)
    $(error Release paths must not contain a semicolon)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),\
    $(if $(findstring $(percent),$($(variable))),$(variable)))),)
    $(error Release paths must not contain a percent sign)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),$(if $(findstring $(hash),$($(variable))),$(variable)))),)
    $(error Release paths must not contain a number sign)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),$(if $(findstring $(dollar),$($(variable))),$(variable)))),)
    $(error Release paths must not contain a dollar sign)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),$(if $(findstring \,$($(variable))),$(variable)))),)
    $(error Release paths must use forward slashes)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),$(if $(findstring //,$($(variable))),$(variable)))),)
    $(error Release paths must not contain empty path components)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),\
    $(if $(filter / . .. ./% ../% %/. %/.. %/,$($(variable))),$(variable)))),)
    $(error Release paths must not contain . or .. path components)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),$(if $(findstring /./,$($(variable))),$(variable)))),)
    $(error Release paths must not contain . path components)
endif
ifneq ($(strip $(foreach variable,$(RELEASE_PATH_VARIABLES),$(if $(findstring /../,$($(variable))),$(variable)))),)
    $(error Release paths must not contain .. path components)
endif

.PHONY: \
    all build debug coverage sanitizers release release-common-assets release-candidate debug-artifact \
    help _build _debug-artifact _release-candidate _release-output _version _release-metadata clean reset-dev-home \
    deps deps-check deps-force deps-clean check-toolchain check-binary \
    check-development check-debug check-coverage check-sanitizers check-release \
    _check-binary quality check docs-assets docs serve version release-metadata \
    validate-release test test-integration test-unit test-unit-build \
    test-helpers _test-helpers test-build test-release test-windows _test-windows \
    test-portability-linux test-coverage test-sanitizers update-ca-bundle \
    check-ca-bundle FORCE

BUILD_RECURSE = $(MAKE) --no-print-directory _build \
    PLATFORM='$(PLATFORM)' DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' \
    CC='$(CC)' WINDRES='$(WINDRES)' \
    CUP_OFFICIAL_BUILD='$(CUP_OFFICIAL_BUILD)'
CUP_INTERNAL_MAKE_FLAG_WORD := $(firstword $(MAKEFLAGS))
CUP_INTERNAL_MAKE_SHORT_FLAGS := $(if $(filter -%,$(CUP_INTERNAL_MAKE_FLAG_WORD)),,$(CUP_INTERNAL_MAKE_FLAG_WORD))
CUP_INTERNAL_MAKE_DRY_RUN := $(findstring n,$(CUP_INTERNAL_MAKE_SHORT_FLAGS)) \
    $(filter -n --just-print --dry-run --recon,$(CUP_INTERNAL_MAKE_FLAG_WORD))
ifneq ($(strip $(CUP_INTERNAL_MAKE_DRY_RUN)),)
BUILD_LOCK_PREFIX =
else
BUILD_LOCK_PREFIX = . '$(PATH_SAFETY)'; cup_path_run_build '$(BUILD_ROOT)' --
endif

all: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(BUILD_RECURSE) CUP_BUILD_CONFIGURATION=development

build: all

debug: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(BUILD_RECURSE) CUP_BUILD_CONFIGURATION=debug

coverage: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(BUILD_RECURSE) CUP_BUILD_CONFIGURATION=coverage

sanitizers: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _build PLATFORM='$(PLATFORM)' \
		DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC=clang WINDRES='$(if $(filter windows-x64,$(PLATFORM)),llvm-windres,$(WINDRES))' \
		CUP_BUILD_CONFIGURATION=sanitizers CUP_OFFICIAL_BUILD=0

release: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(BUILD_RECURSE) CUP_BUILD_CONFIGURATION=release

# Common assets and native candidates are generated only below the managed
# build root. Official candidates consume an already verified dependency prefix.
release-common-assets: $(BUILD_ROOT_MARKER)
	@$(BUILD_LOCK_PREFIX) env VERSION='$(CUP_RELEASE_VERSION)' TAG='$(CUP_RELEASE_TAG)' SHA='$(CUP_RELEASE_COMMIT)' \
		SOURCE_REPOSITORY='$(SOURCE_REPOSITORY)' TESTS_RUN_ID='$(TESTS_RUN_ID)' \
		TESTS_RUN_ATTEMPT='$(TESTS_RUN_ATTEMPT)' \
		TESTS_EVIDENCE_INDEX_SHA256='$(TESTS_EVIDENCE_INDEX_SHA256)' \
		RELEASE_RUN_ID='$(RELEASE_RUN_ID)' RELEASE_RUN_ATTEMPT='$(RELEASE_RUN_ATTEMPT)' \
		CUP_BUILD_ROOT='$(BUILD_ROOT)' \
		./scripts/release/common-assets.sh '$(abspath $(RELEASE_COMMON_ROOT))'

release-candidate: deps-check | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _release-output \
		PLATFORM='$(PLATFORM)' DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' WINDRES='$(WINDRES)' CUP_BUILD_CONFIGURATION=release \
		CUP_OFFICIAL_BUILD=1 CUP_RELEASE_VERSION='$(CUP_RELEASE_VERSION)' \
		CUP_RELEASE_TAG='$(CUP_RELEASE_TAG)' CUP_RELEASE_COMMIT='$(CUP_RELEASE_COMMIT)'

_release-output: _release-candidate
	@test -d '$(RELEASE_COMMON_DIR)' && test ! -L '$(RELEASE_COMMON_DIR)' || { \
		echo 'Release common assets are missing; run make release-common-assets or set RELEASE_COMMON_DIR.' >&2; exit 1; }
	@VERSION='$(CUP_RELEASE_VERSION)' TAG='$(CUP_RELEASE_TAG)' SHA='$(CUP_RELEASE_COMMIT)' \
		PLATFORM='$(PLATFORM)' CUP_BUILD_ROOT='$(BUILD_ROOT)' \
		./scripts/release/build-platform.sh '$(abspath $(RELEASE_COMMON_DIR))' \
		'$(abspath $(FINALIZED_ROOT))' '$(abspath $(RELEASE_PLATFORM_ROOT))'

debug-artifact: deps-check | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _debug-artifact \
		PLATFORM='$(PLATFORM)' DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' WINDRES='$(WINDRES)' CUP_BUILD_CONFIGURATION=debug \
		CUP_OFFICIAL_BUILD=0


help:
	@printf '%s\n' \
		'Build targets:' \
		'  make, make build             development build' \
		'  make debug                   diagnostic build with rich symbols' \
		'  make coverage                coverage-instrumented build' \
		'  make sanitizers              ASan/UBSan build for the native platform' \
		'  make release                 non-finalized release-configuration build' \
		'  make release-common-assets   exact shared installer/catalog release assets' \
		'  make release-candidate       official finalized candidate (prepared deps)' \
		'  make debug-artifact          executable plus native debug symbols' \
		'  make clean                   remove an owned build root' \
		'  make help                    list all public Make targets' \
		'' \
		'Dependency and toolchain targets:' \
		'  JOBS=4 make deps             reuse or build the pinned dependency prefix' \
		'  make deps-check              validate without modifying the prefix' \
		'  make deps-force              rebuild the prefix transactionally' \
		'  make deps-clean              remove an owned dependency root' \
		'  make check-toolchain         validate the selected native toolchain' \
		'  make check-binary            development build and inspection' \
		'  make check-{debug,coverage,sanitizers,release}' \
		'                               inspect the named configuration' \
		'' \
		'Test and quality targets:' \
		'  make test                    unit and native integration tests' \
		'  make test-unit               C unit tests' \
		'  make test-integration        native integration tests' \
		'  make quality                 repository and release-script quality checks' \
		'  make check                   dependencies, tests and quality checks' \
		'  make test-coverage           coverage gate, report and binary inspection' \
		'  make test-sanitizers         ASan/UBSan gate and binary inspection' \
		'  make test-portability-linux  Linux static runtime portability test' \
		'  make test-windows            Windows unit and integration tests' \
		'  make test-release RELEASE_DIR=<dir>' \
		'                               validate an unpacked release candidate' \
		'' \
		'Version, certificate and documentation targets:' \
		'  make version                 print generated build identity' \
		'  make validate-release        validate the explicit/local release context' \
		'  make release-metadata        print generated version metadata path' \
		'  make check-ca-bundle         validate the checked-in CA bundle' \
		'  make update-ca-bundle        download and validate a new CA bundle' \
		'  make docs-assets             explicitly refresh the remote theme asset' \
		'  make docs                    refresh theme assets and build documentation' \
		'  make serve                   refresh theme assets and serve documentation' \
		'' \
		'Maintenance:' \
		'  CUP_ALLOW_DEV_CLEAN=1 make reset-dev-home' \
		'                               remove build outputs and the marked dev root' \
		'Local additions: EXTRA_CPPFLAGS, EXTRA_CFLAGS, EXTRA_LDFLAGS, EXTRA_LDLIBS' \
		'Current platform: $(PLATFORM)' \
		'Supported platforms: $(SUPPORTED_PLATFORM)'

_build: $(TARGET)

$(BUILD_ROOT_MARKER):
	@root='$(BUILD_ROOT)'; project='$(PROJECT_ROOT)'; \
	. '$(PATH_SAFETY)'; \
	cup_path_validate_absolute_clean "$$root" 'BUILD_DIR' || exit 1; \
	if test "$$root" = "$$project"; then \
		echo 'BUILD_DIR cannot be the project root.' >&2; \
		exit 1; \
	fi; \
	cup_path_prepare_build_root "$$root"

deps:
	@case "$(PLATFORM)" in \
		linux-*|macos-*) builder=./scripts/dependencies/build-posix.sh ;; \
		windows-x64) builder=./scripts/dependencies/build-windows.sh ;; \
		*) echo "Unsupported PLATFORM '$(PLATFORM)' for dependencies." >&2; exit 1 ;; \
	esac; \
	JOBS='$(JOBS)' PLATFORM='$(PLATFORM)' DEPS_ROOT='$(DEPS_ROOT)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' MACOSX_DEPLOYMENT_TARGET='$(MACOSX_DEPLOYMENT_TARGET)' \
		bash "$$builder"

deps-check:
	@CUP_DEPENDENCY_PROFILE='$(CUP_DEPENDENCY_PROFILE)' \
		./scripts/dependencies/verify.sh '$(PLATFORM)' '$(DEPS_PREFIX)'

deps-force:
	+@CUP_DEPS_FORCE=1 $(MAKE) --no-print-directory deps \
		PLATFORM='$(PLATFORM)' DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)'

deps-clean:
	@./scripts/dependencies/verify.sh --clean-root '$(DEPS_ROOT)'

check-toolchain:
	@./scripts/build/validate-toolchain.sh \
		'$(PLATFORM)' '$(CC)' '$(WINDRES)' '$(CONFIGURATION)' \
		'$(CUP_INTERNAL_TOOLCHAIN_ROLE)'

check-binary: check-development
check-development: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _check-binary PLATFORM='$(PLATFORM)' \
		DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' WINDRES='$(WINDRES)' \
		CUP_BUILD_CONFIGURATION=development CUP_OFFICIAL_BUILD=0
check-debug: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _check-binary PLATFORM='$(PLATFORM)' \
		DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' WINDRES='$(WINDRES)' \
		CUP_BUILD_CONFIGURATION=debug CUP_OFFICIAL_BUILD=0
check-coverage: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _check-binary PLATFORM='$(PLATFORM)' \
		DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' WINDRES='$(WINDRES)' \
		CUP_BUILD_CONFIGURATION=coverage CUP_OFFICIAL_BUILD=0
check-sanitizers: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _check-binary PLATFORM='$(PLATFORM)' \
		DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' CC=clang \
		WINDRES='$(if $(filter windows-x64,$(PLATFORM)),llvm-windres,$(WINDRES))' \
		CUP_BUILD_CONFIGURATION=sanitizers CUP_OFFICIAL_BUILD=0
check-release: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _check-binary PLATFORM='$(PLATFORM)' \
		DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' WINDRES='$(WINDRES)' \
		CUP_BUILD_CONFIGURATION=release CUP_OFFICIAL_BUILD=0

_check-binary: $(BINARY_INSPECTION)
	@cat "$(BINARY_INSPECTION)"

$(BINARY_INSPECTION): $(TARGET) scripts/build/inspect-binary.sh | $(BUILD_ROOT_MARKER)
	@CUP_BUILD_ROOT='$(BUILD_ROOT)' ./scripts/build/inspect-binary.sh \
		'$(PLATFORM)' '$(CONFIGURATION)' '$(abspath $(TARGET))' '$(abspath $@)' build

_debug-artifact: $(TARGET) $(BUILD_CONFIG) $(VERSION_METADATA) | $(BUILD_ROOT_MARKER)
	@CUP_BUILD_ROOT='$(BUILD_ROOT)' ./scripts/build/finalize-release.sh \
		'$(PLATFORM)' debug '$(abspath $(TARGET))' '$(abspath $(FINALIZED_ROOT))' \
		'$(abspath $(BUILD_CONFIG))' '$(abspath $(VERSION_METADATA))' build \
		'$(abspath scripts/build/inspect-binary.sh)' \
		'$(abspath scripts/build/check-path-leaks.sh)'
	@printf '%s\n' '$(FINALIZED_ROOT)'

_release-candidate: $(TARGET) $(BUILD_CONFIG) $(VERSION_METADATA) | $(BUILD_ROOT_MARKER)
	@CUP_BUILD_ROOT='$(BUILD_ROOT)' ./scripts/build/finalize-release.sh \
		'$(PLATFORM)' release '$(abspath $(TARGET))' '$(abspath $(FINALIZED_ROOT))' \
		'$(abspath $(BUILD_CONFIG))' '$(abspath $(VERSION_METADATA))' public \
		'$(abspath scripts/build/inspect-binary.sh)' \
		'$(abspath scripts/build/check-path-leaks.sh)' \
		'$(PROJECT_ROOT)' '$(DEPS_PREFIX)' '$(HOME)' '$(BUILD_ROOT)'
	@printf '%s\n' '$(FINALIZED_ROOT)'

FORCE:

$(VERSION_STAMP): FORCE VERSION scripts/version.sh | $(BUILD_ROOT_MARKER)
	@. '$(PATH_SAFETY)'; \
		cup_path_prepare_child_directory '$(BUILD_ROOT)' '$(abspath $(GENERATED_DIR))' 'generated directory'
	@CUP_BUILD_ROOT='$(BUILD_ROOT)' CUP_OFFICIAL_BUILD='$(VERSION_OFFICIAL_BUILD)' \
		CUP_BUILD_CONFIGURATION='$(CONFIGURATION)' \
		CUP_RELEASE_VERSION='$(CUP_RELEASE_VERSION)' \
		CUP_RELEASE_TAG='$(CUP_RELEASE_TAG)' \
		CUP_RELEASE_COMMIT='$(CUP_RELEASE_COMMIT)' \
		./scripts/version.sh generate "$(GENERATED_DIR)"
	@. '$(PATH_SAFETY)'; : | cup_path_write_file '$(abspath $@)' 0644 replace

$(VERSION_HEADER) $(VERSION_RESOURCE) $(VERSION_METADATA): $(VERSION_STAMP)
	@if test ! -f "$@"; then \
		. '$(PATH_SAFETY)'; \
		cup_path_remove_file '$(abspath $(VERSION_STAMP))' 'version stamp'; \
		$(MAKE) "$(VERSION_STAMP)"; \
	fi

$(CA_BUNDLE_STAMP): certs/cacert.pem certs/cacert.meta \
        scripts/certs/check-ca-bundle.sh scripts/certs/generate-ca-bundle.sh | $(BUILD_ROOT_MARKER)
	@. '$(PATH_SAFETY)'; \
		cup_path_prepare_child_directory '$(BUILD_ROOT)' '$(abspath $(GENERATED_DIR))' 'generated directory'
	@CUP_BUILD_ROOT='$(BUILD_ROOT)' ./scripts/certs/generate-ca-bundle.sh certs/cacert.pem "$(GENERATED_DIR)"
	@. '$(PATH_SAFETY)'; : | cup_path_write_file '$(abspath $@)' 0644 replace

$(CA_BUNDLE_HEADER) $(CA_BUNDLE_SOURCE): $(CA_BUNDLE_STAMP)
	@if test ! -f "$@"; then \
		. '$(PATH_SAFETY)'; \
		cup_path_remove_file '$(abspath $(CA_BUNDLE_STAMP))' 'CA bundle stamp'; \
		$(MAKE) "$(CA_BUNDLE_STAMP)"; \
	fi

$(BUILD_CONFIG): FORCE Makefile scripts/build/write-config.sh | $(BUILD_ROOT_MARKER) deps-check check-toolchain
	@CUP_BUILD_PLATFORM='$(PLATFORM)' CUP_BUILD_CONFIGURATION='$(CONFIGURATION)' \
		CUP_BUILD_CC='$(CC)' CUP_BUILD_WINDRES='$(WINDRES)' \
		CUP_BUILD_CPPFLAGS='$(CPPFLAGS)' CUP_BUILD_CFLAGS='$(CFLAGS)' \
		CUP_BUILD_LDFLAGS='$(LDFLAGS)' CUP_BUILD_LDLIBS='$(LDLIBS)' \
		CUP_BUILD_DEPS_PREFIX='$(DEPS_PREFIX)' CUP_BUILD_OFFICIAL='$(CUP_OFFICIAL_BUILD)' \
		CUP_BUILD_ROOT='$(BUILD_ROOT)' ./scripts/build/write-config.sh '$(abspath $@)'

$(OBJ): $(BUILD_CONFIG) $(PROJECT_HEADERS) $(PROJECT_HEADER_DIRS) $(VERSION_HEADER) \
        $(CA_BUNDLE_HEADER) Makefile | $(VERSION_STAMP) $(CA_BUNDLE_STAMP)

$(TARGET): $(OBJ) $(BUILD_CONFIG) Makefile
	@. '$(PATH_SAFETY)'; cup_path_prepare_child_directory '$(BUILD_ROOT)' '$(abspath $(BIN_DIR))' 'binary directory'
	$(CC) $(CFLAGS) $(CPPFLAGS) $(OBJ) -o "$@" $(LDFLAGS) $(LDLIBS)

$(CA_BUNDLE_OBJ): $(CA_BUNDLE_SOURCE) $(CA_BUNDLE_HEADER) $(BUILD_CONFIG)
	@. '$(PATH_SAFETY)'; cup_path_prepare_child_directory '$(BUILD_ROOT)' '$(abspath $(dir $@))' 'object directory'
	$(CC) $(CFLAGS) $(CPPFLAGS) -c "$(CA_BUNDLE_SOURCE)" -o "$@"

ifneq ($(strip $(COVERAGE_ENTRY_OBJ)),)
$(COVERAGE_ENTRY_OBJ): tests/helpers/coverage-entry.c $(BUILD_CONFIG)
	@. '$(PATH_SAFETY)'; cup_path_prepare_child_directory '$(BUILD_ROOT)' '$(abspath $(dir $@))' 'object directory'
	$(CC) $(CFLAGS) $(CPPFLAGS) -c "$<" -o "$@"
endif

$(OBJ_DIR)/%.o: src/%.c $(BUILD_CONFIG)
	@. '$(PATH_SAFETY)'; cup_path_prepare_child_directory '$(BUILD_ROOT)' '$(abspath $(dir $@))' 'object directory'
	$(CC) $(CFLAGS) $(CPPFLAGS) -c "$<" -o "$@"

ifeq ($(PLATFORM),windows-x64)
$(RESOURCE_OBJ): $(VERSION_RESOURCE) $(VERSION_HEADER) $(BUILD_CONFIG) Makefile
	@. '$(PATH_SAFETY)'; \
		cup_path_prepare_child_directory '$(BUILD_ROOT)' '$(abspath $(dir $@))' 'resource object directory'
	$(WINDRES) -I$(call escape_spaces,$(GENERATED_DIR)) "$<" -O coff -o "$@"
endif

version: | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _version \
		PLATFORM='$(PLATFORM)' DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' WINDRES='$(WINDRES)'

_version: $(VERSION_STAMP)
	@cat "$(VERSION_METADATA)"

validate-release:
	@CUP_OFFICIAL_BUILD=1 CUP_BUILD_CONFIGURATION=release \
		CUP_RELEASE_VERSION='$(CUP_RELEASE_VERSION)' CUP_RELEASE_TAG='$(CUP_RELEASE_TAG)' \
		CUP_RELEASE_COMMIT='$(CUP_RELEASE_COMMIT)' ./scripts/version.sh validate-release

release-metadata: | $(BUILD_ROOT_MARKER)
	+@$(BUILD_LOCK_PREFIX) $(MAKE) --no-print-directory _release-metadata \
		PLATFORM='$(PLATFORM)' DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' WINDRES='$(WINDRES)'

_release-metadata: $(VERSION_STAMP)
	@printf '%s\n' '$(VERSION_METADATA)'

clean:
	@root='$(BUILD_ROOT)'; project='$(PROJECT_ROOT)'; \
	if test ! -e "$$root" && test ! -L "$$root"; then \
		exit 0; \
	fi; \
	. '$(PATH_SAFETY)'; \
	cup_path_validate_absolute_clean "$$root" 'BUILD_DIR' || exit 1; \
	if test "$$root" = "$$project"; then \
		echo 'Refusing to remove the project root.' >&2; \
		exit 1; \
	fi; \
	case "$$project/" in \
		"$$root/"*) \
			echo 'Refusing to remove an ancestor of the project root.' >&2; \
			exit 1; \
			;; \
	esac; \
	cup_path_clean_build_root "$$root"

reset-dev-home:
	@if test "$(CUP_ALLOW_DEV_CLEAN)" != "1"; then \
		echo "Refusing to remove the dev root without CUP_ALLOW_DEV_CLEAN=1" >&2; \
		exit 1; \
	fi
	+@$(MAKE) --no-print-directory clean BUILD_DIR='$(BUILD_DIR)'
	@case "$(HOME)" in \
		/*) test "$(HOME)" != "/" ;; \
		*) false ;; \
	esac || { \
		echo "Invalid HOME for reset-dev-home" >&2; \
		exit 1; \
	}
	@selected_root=; \
	. '$(PATH_SAFETY)'; \
	cup_path_validate_absolute_clean '$(HOME)' 'HOME' || exit 1; \
	cup_path_check_directory_chain '$(HOME)' 0 'HOME' || exit 1; \
	for candidate_root in "$(HOME)/.cup" "$(HOME)/.coffee-cup"; do \
		marker_path="$$candidate_root/root.txt"; \
		if test -e "$$candidate_root" || test -L "$$candidate_root"; then \
			cup_path_check_directory_chain \
				"$$candidate_root" 0 'development root candidate' || exit 1; \
		fi; \
		if test -d "$$candidate_root" && \
			cup_path_require_regular_file \
				"$$marker_path" 'development root marker' >/dev/null 2>&1 && \
			test "$$(awk 'END { print NR }' "$$marker_path")" = 3 && \
			test "$$(sed -n '1p' "$$marker_path")" = 'format=1' && \
			test "$$(sed -n '2p' "$$marker_path")" = 'product=coffee-clang/cup' && \
			test "$$(sed -n '3p' "$$marker_path")" = 'layout=1'; then \
			if test -n "$$selected_root"; then \
				echo "Both dev root candidates are marked as cup roots." >&2; \
				exit 1; \
			fi; \
			selected_root="$$candidate_root"; \
		fi; \
	done; \
	if test -z "$$selected_root"; then \
		echo "No marked cup dev root was found; nothing was removed." >&2; \
		exit 1; \
	fi; \
	cup_path_remove_child_tree \
		'$(HOME)' "$$selected_root" 'development root' || exit 1

# Platform wrappers choose their platform before validating prepared dependencies.
CUP_TEST_CONFIGURATION ?= development

test-unit-build: $(CUP_INTERNAL_DEPS_TARGET) | $(BUILD_ROOT_MARKER)
	@$(BUILD_LOCK_PREFIX) env CUP_TEST_PLATFORM='$(PLATFORM)' CUP_TEST_CONFIGURATION='$(CUP_TEST_CONFIGURATION)' \
		CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		CUP_TEST_CPPFLAGS='$(strip $(TEST_CPPFLAGS))' \
		CUP_TEST_CFLAGS='$(strip $(TEST_CFLAGS))' \
		CUP_TEST_LDFLAGS='$(strip $(TEST_LDFLAGS))' ./tests/build/unit.sh

test-helpers: $(CUP_INTERNAL_DEPS_TARGET)
	+@$(MAKE) --no-print-directory _test-helpers PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		CUP_TEST_CONFIGURATION='$(CUP_TEST_CONFIGURATION)'

_test-helpers: deps-check | $(BUILD_ROOT_MARKER)
	@$(BUILD_LOCK_PREFIX) env CUP_TEST_PLATFORM='$(PLATFORM)' CUP_TEST_CONFIGURATION='$(CUP_TEST_CONFIGURATION)' \
		CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		CUP_TEST_CPPFLAGS='$(strip $(TEST_CPPFLAGS))' \
		CUP_TEST_CFLAGS='$(strip $(TEST_CFLAGS))' \
		CUP_TEST_LDFLAGS='$(strip $(TEST_LDFLAGS))' ./tests/build/helpers.sh

test-build: $(CUP_INTERNAL_DEPS_TARGET)
	+@$(MAKE) --no-print-directory all PLATFORM='$(PLATFORM)' DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)'
	+@$(MAKE) --no-print-directory test-unit-build PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' CUP_TEST_CONFIGURATION=development
	+@$(MAKE) --no-print-directory test-helpers PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' CUP_TEST_CONFIGURATION=development

test: $(CUP_INTERNAL_DEPS_TARGET)
	@case '$(PLATFORM)' in \
		windows-x64) \
			$(MAKE) --no-print-directory _test-windows \
				PLATFORM=windows-x64 \
				DEPS_ROOT='$(DEPS_ROOT)' \
				DEPS_PREFIX='$(DEPS_PREFIX)' \
				CC='$(CC)' \
			;; \
		*) \
			$(MAKE) --no-print-directory test-build \
				PLATFORM='$(PLATFORM)' \
				DEPS_PREFIX='$(DEPS_PREFIX)' \
				CC='$(CC)' && \
			env \
				CUP_TEST_PLATFORM='$(PLATFORM)' \
				CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' \
				DEPS_PREFIX='$(DEPS_PREFIX)' \
				CC='$(CC)' \
				CUP_TEST_CONFIGURATION=development \
				./tests/runners/all-posix.sh \
			;; \
	esac

test-unit: $(CUP_INTERNAL_DEPS_TARGET)
	+@$(MAKE) --no-print-directory test-unit-build \
		PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' \
		CUP_TEST_CONFIGURATION=development
	@env \
		CUP_TEST_PLATFORM='$(PLATFORM)' \
		CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' \
		CUP_TEST_CONFIGURATION=development \
		./tests/runners/unit.sh

test-integration: $(CUP_INTERNAL_DEPS_TARGET)
	+@$(MAKE) --no-print-directory all \
		PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)'
	+@$(MAKE) --no-print-directory test-helpers \
		PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' \
		CUP_TEST_CONFIGURATION=development
	@case '$(PLATFORM)' in \
		windows-x64) \
			CUP_TEST_BUILD_ROOT="$$(cygpath -w '$(BUILD_ROOT)')" \
			powershell.exe -NoProfile -ExecutionPolicy Bypass \
				-File "$$(cygpath -w '$(PROJECT_ROOT)/tests/runners/integration-windows.ps1')" \
				-CupPath "$$(cygpath -w '$(BUILD_ROOT)/windows-x64/development/bin/cup.exe')" \
				-Configuration development \
			;; \
		*) \
			env \
				CUP_TEST_PLATFORM='$(PLATFORM)' \
				CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' \
				DEPS_PREFIX='$(DEPS_PREFIX)' \
				CC='$(CC)' \
				CUP_TEST_CONFIGURATION=development \
				./tests/runners/integration-posix.sh \
			;; \
	esac

quality:
	@env \
		CUP_TEST_PLATFORM='$(PLATFORM)' \
		CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' \
		DEPS_ROOT='$(DEPS_ROOT)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' \
		./tests/runners/repository.sh

check:
	+@$(MAKE) --no-print-directory test \
		PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)'
	+@CUP_TEST_WITH_BUILD_OUTPUT=1 $(MAKE) --no-print-directory quality \
		PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)'

test-release: deps-check
	@test -n "$(RELEASE_DIR)" || { echo "Set RELEASE_DIR=<candidate-dir>" >&2; exit 2; }
	+@$(MAKE) --no-print-directory _test-helpers \
		PLATFORM='$(PLATFORM)' \
		DEPS_ROOT='$(DEPS_ROOT)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' \
		CUP_TEST_CONFIGURATION='$(CUP_TEST_CONFIGURATION)'
	@set -e; \
	version=$$(cat VERSION); \
	sha=$$(git -C '$(PROJECT_ROOT)' rev-parse HEAD); \
	server_root='$(BUILD_ROOT)/release-test-server-$(PLATFORM)'; \
	env PLATFORM='$(PLATFORM)' VERSION="$$version" SHA="$$sha" \
		DEPS_ROOT='$(DEPS_ROOT)' DEPS_PREFIX='$(DEPS_PREFIX)' CC='$(CC)' \
		WINDRES='$(WINDRES)' CUP_BUILD_ROOT='$(BUILD_ROOT)' CUP_BUILD_DIR='$(BUILD_DIR)' \
		./tests/release/update-fixture.sh "$(RELEASE_DIR)" "$$server_root" >/dev/null; \
	case '$(PLATFORM)' in \
		windows-x64) \
			CUP_TEST_BUILD_ROOT="$$(cygpath -w '$(BUILD_ROOT)')" \
			CUP_TEST_SERVER_ROOT="$$(cygpath -w "$$server_root")" \
			CUP_TEST_CONFIGURATION='$(CUP_TEST_CONFIGURATION)' \
			powershell.exe -NoProfile -ExecutionPolicy Bypass \
				-File "$$(cygpath -w '$(PROJECT_ROOT)/tests/release/windows.ps1')" \
				-ReleaseDir "$$(cygpath -w '$(RELEASE_DIR)')" \
				-Version "$$version" \
				-SourceSha "$$sha" \
			;; \
		*) \
			env \
				PLATFORM='$(PLATFORM)' \
				VERSION="$$version" \
				SHA="$$sha" \
				CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' \
				CUP_TEST_SERVER_ROOT="$$server_root" \
				CUP_TEST_CONFIGURATION='$(CUP_TEST_CONFIGURATION)' \
				./tests/release/posix.sh "$(RELEASE_DIR)" \
			;; \
	esac

test-coverage: $(CUP_INTERNAL_DEPS_TARGET)
	@env \
		CUP_TEST_PLATFORM='$(PLATFORM)' \
		CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		./tests/runners/coverage.sh
	+@$(MAKE) --no-print-directory _check-binary \
		PLATFORM='$(PLATFORM)' \
		DEPS_ROOT='$(DEPS_ROOT)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' \
		WINDRES='$(WINDRES)' \
		CUP_BUILD_CONFIGURATION=coverage \
		CUP_OFFICIAL_BUILD=0

test-sanitizers: $(CUP_INTERNAL_DEPS_TARGET)
	@env \
		CUP_TEST_PLATFORM='$(PLATFORM)' \
		CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		./tests/runners/sanitizers.sh
	+@$(MAKE) --no-print-directory _check-binary \
		PLATFORM='$(PLATFORM)' \
		DEPS_ROOT='$(DEPS_ROOT)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC=clang \
		WINDRES='$(if $(filter windows-x64,$(PLATFORM)),llvm-windres,$(WINDRES))' \
		CUP_BUILD_CONFIGURATION=sanitizers \
		CUP_OFFICIAL_BUILD=0

test-portability-linux: $(CUP_INTERNAL_DEPS_TARGET)
	@case '$(PLATFORM)' in \
		linux-x64|linux-arm64) \
			;; \
		*) \
			echo 'test-portability-linux requires a native Linux PLATFORM.' >&2; \
			exit 2 \
			;; \
	esac
	@PLATFORM='$(PLATFORM)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		./tests/portability/linux-static-runtime.sh

test-windows:
	+@$(MAKE) --no-print-directory _test-windows \
		PLATFORM=windows-x64 \
		DEPS_ROOT='$(HOME)/deps/windows-x64' \
		DEPS_PREFIX='$(HOME)/deps/windows-x64/install' \
		CC=gcc \
		WINDRES=windres

_test-windows: $(CUP_INTERNAL_DEPS_TARGET)
	+@$(MAKE) --no-print-directory test-build \
		PLATFORM=windows-x64 \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)'
	@env \
		CUP_TEST_PLATFORM=windows-x64 \
		CUP_TEST_BUILD_ROOT='$(BUILD_ROOT)' \
		DEPS_PREFIX='$(DEPS_PREFIX)' \
		CC='$(CC)' \
		CUP_TEST_CONFIGURATION=development \
		./tests/runners/unit.sh
	@CUP_TEST_BUILD_ROOT="$$(cygpath -w '$(BUILD_ROOT)')" \
		powershell.exe -NoProfile -ExecutionPolicy Bypass \
			-File "$$(cygpath -w '$(PROJECT_ROOT)/tests/runners/integration-windows.ps1')" \
			-CupPath "$$(cygpath -w '$(BUILD_ROOT)/windows-x64/development/bin/cup.exe')" \
			-Configuration development

check-ca-bundle:
	@./scripts/certs/check-ca-bundle.sh

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
