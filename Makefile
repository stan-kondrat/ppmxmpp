# ppmxmpp Makefile

.DEFAULT_GOAL := all

BUILD       ?= debug
SRCDIR      := src
INCDIR      := include
BUILDDIR    := build/$(BUILD)
THIRDPARTY  := third_party
TPBUILD     := $(BUILDDIR)/third_party

# ---------------------------------------------------------------------------
# Per-library static/shared toggles (YES/NO)
# ---------------------------------------------------------------------------
MBEDTLS_STATIC    ?= YES
MBEDTLS_SHARED    ?= NO
LIBUV_STATIC      ?= YES
LIBUV_SHARED      ?= NO
LIBSTROPHE_STATIC ?= YES
LIBSTROPHE_SHARED ?= NO
SQLITE_STATIC     ?= YES
SQLITE_SHARED     ?= NO
STUMPLESS_STATIC  ?= YES
STUMPLESS_SHARED  ?= NO
CMOCKA_STATIC     ?= YES
CMOCKA_SHARED     ?= NO
LIBCONFIG_STATIC  ?= YES
LIBCONFIG_SHARED  ?= NO

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
define linktype
$(if $(filter YES,$(1)),$(if $(filter YES,$(2)),static+shared,static),shared)
endef

TP_LINK_DEFINES := \
    -DTP_MBEDTLS_LINK=\"$(call linktype,$(MBEDTLS_STATIC),$(MBEDTLS_SHARED))\" \
    -DTP_LIBUV_LINK=\"$(call linktype,$(LIBUV_STATIC),$(LIBUV_SHARED))\" \
    -DTP_LIBSTROPHE_LINK=\"$(call linktype,$(LIBSTROPHE_STATIC),$(LIBSTROPHE_SHARED))\" \
    -DTP_SQLITE_LINK=\"$(call linktype,$(SQLITE_STATIC),$(SQLITE_SHARED))\" \
    -DTP_STUMPLESS_LINK=\"$(call linktype,$(STUMPLESS_STATIC),$(STUMPLESS_SHARED))\" \
    -DTP_CMOCKA_LINK=\"$(call linktype,$(CMOCKA_STATIC),$(CMOCKA_SHARED))\" \
    -DTP_LIBCONFIG_LINK=\"$(call linktype,$(LIBCONFIG_STATIC),$(LIBCONFIG_SHARED))\"

TP_CFLAGS   := \
    $(TP_LINK_DEFINES) \
    -I$(THIRDPARTY)/mbedtls/include \
    -I$(THIRDPARTY)/mbedtls/tf-psa-crypto/include \
    -I$(THIRDPARTY)/mbedtls/tf-psa-crypto/drivers/builtin/include \
    -I$(THIRDPARTY)/libuv/include \
    -I$(THIRDPARTY)/libstrophe \
    -I$(THIRDPARTY)/sqlite \
    -I$(THIRDPARTY)/stumpless/include \
    -I$(TPBUILD)/cmocka/include \
    -I$(TPBUILD)/stumpless/include \
    -I$(THIRDPARTY)/libconfig/lib

TP_LDFLAGS  := \
    $(TPBUILD)/mbedtls/libmbedtls.a \
    $(TPBUILD)/mbedtls/libmbedx509.a \
    $(TPBUILD)/mbedtls/libtfpsacrypto.a \
    $(TPBUILD)/libuv/libuv.a \
    $(TPBUILD)/libstrophe/lib/libstrophe.a \
    $(THIRDPARTY)/sqlite/libsqlite3.a \
    $(TPBUILD)/stumpless/libstumpless.a \
    $(TPBUILD)/cmocka/libcmocka.a \
    $(TPBUILD)/libconfig/out/libconfig.a \
    -lpthread -ldl -lm

CFLAGS      := -std=c2x -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
               -Wstrict-prototypes -Wmissing-prototypes -MMD -MP \
               -I$(INCDIR) $(TP_CFLAGS)

ifeq ($(BUILD),debug)
  CFLAGS += -g -O0
else ifeq ($(BUILD),release)
  CFLAGS += -O2 -DNDEBUG
else ifeq ($(BUILD),asan)
  CFLAGS += -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS += -fsanitize=address,undefined
endif

LDFLAGS     ?=

# ---------------------------------------------------------------------------
# Project sources
# ---------------------------------------------------------------------------
SRCS        := $(wildcard $(SRCDIR)/*.c) $(wildcard $(SRCDIR)/storage/*.c)
OBJS        := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

# Embed default config as a C string literal, rebuilt when the file changes
DEFAULT_CONFIG_SRC := $(BUILDDIR)/default_config.c
DEFAULT_CONFIG_OBJ := $(BUILDDIR)/default_config.o
OBJS                += $(DEFAULT_CONFIG_OBJ)

DEPS        := $(OBJS:.o=.d)
TARGET      := $(BUILDDIR)/ppmxmpp

$(DEFAULT_CONFIG_SRC): config/ppmxmpp.conf | $(BUILDDIR)
	@{ \
	  printf '/* generated from config/ppmxmpp.conf — do not edit */\n'; \
	  printf '#include "config.h"\n\n'; \
	  printf 'const char DEFAULT_CONFIG_CONTENT[] =\n'; \
	  sed 's/\\/\\\\/g; s/"/\\"/g; s/.*/    "&\\n"/' $<; \
	  printf '    ;\n'; \
	} > $@

$(DEFAULT_CONFIG_OBJ): $(DEFAULT_CONFIG_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---------------------------------------------------------------------------
# cmake helper: $(call cmake_build, SRC_DIR, BUILD_DIR, EXTRA_DEFS)
# ---------------------------------------------------------------------------
define cmake_build
	cmake -S $(1) -B $(2) $(3) \
	      -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=$(abspath $(2)) \
	      -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$(abspath $(2))
	cmake --build $(2) --parallel
endef

# ---------------------------------------------------------------------------
# mbedtls  (CMake)
# ---------------------------------------------------------------------------
MBEDTLS_SRC   := $(THIRDPARTY)/mbedtls
MBEDTLS_BUILD := $(TPBUILD)/mbedtls
MBEDTLS_STAMP := $(MBEDTLS_BUILD)/.built
MBEDTLS_DEFS  :=
ifeq ($(MBEDTLS_STATIC),YES)
  MBEDTLS_DEFS += -DUSE_STATIC_MBEDTLS_LIBRARY=ON
endif
ifeq ($(MBEDTLS_SHARED),YES)
  MBEDTLS_DEFS += -DUSE_SHARED_MBEDTLS_LIBRARY=ON
endif

$(MBEDTLS_STAMP):
	mkdir -p $(MBEDTLS_BUILD)
	$(call cmake_build,$(MBEDTLS_SRC),$(MBEDTLS_BUILD),$(MBEDTLS_DEFS))
	@touch $@

.PHONY: mbedtls
mbedtls: $(MBEDTLS_STAMP)

# ---------------------------------------------------------------------------
# libuv  (CMake)
# ---------------------------------------------------------------------------
LIBUV_SRC   := $(THIRDPARTY)/libuv
LIBUV_BUILD := $(TPBUILD)/libuv
LIBUV_STAMP := $(LIBUV_BUILD)/.built
ifeq ($(LIBUV_SHARED),YES)
  LIBUV_SHARED_FLAG := ON
else
  LIBUV_SHARED_FLAG := OFF
endif

$(LIBUV_STAMP):
	mkdir -p $(LIBUV_BUILD)
	$(call cmake_build,$(LIBUV_SRC),$(LIBUV_BUILD),-DLIBUV_BUILD_SHARED=$(LIBUV_SHARED_FLAG))
	@touch $@

.PHONY: libuv
libuv: $(LIBUV_STAMP)

# ---------------------------------------------------------------------------
# libstrophe  (Autotools)
# ---------------------------------------------------------------------------
LIBSTROPHE_SRC   := $(THIRDPARTY)/libstrophe
LIBSTROPHE_BUILD := $(TPBUILD)/libstrophe
LIBSTROPHE_STAMP := $(LIBSTROPHE_BUILD)/.built
ifeq ($(LIBSTROPHE_STATIC),YES)
  ifeq ($(LIBSTROPHE_SHARED),YES)
    LIBSTROPHE_FLAGS := --enable-static --enable-shared
  else
    LIBSTROPHE_FLAGS := --enable-static --disable-shared
  endif
else
  LIBSTROPHE_FLAGS := --disable-static --enable-shared
endif

$(LIBSTROPHE_STAMP):
	mkdir -p $(LIBSTROPHE_BUILD)
	cd $(LIBSTROPHE_SRC) && test -f configure || autoreconf -fi
	cd $(LIBSTROPHE_BUILD) && $(abspath $(LIBSTROPHE_SRC))/configure \
	    --prefix=$(abspath $(LIBSTROPHE_BUILD)) $(LIBSTROPHE_FLAGS)
	$(MAKE) -C $(LIBSTROPHE_BUILD) install
	@touch $@

.PHONY: libstrophe
libstrophe: $(LIBSTROPHE_STAMP)

# ---------------------------------------------------------------------------
# sqlite  (Autotools / amalgamation)
# ---------------------------------------------------------------------------
SQLITE_SRC   := $(THIRDPARTY)/sqlite
SQLITE_STAMP := $(SQLITE_SRC)/.built
ifeq ($(SQLITE_SHARED),YES)
  ifeq ($(SQLITE_STATIC),YES)
    SQLITE_CONFIGURE_FLAGS := --enable-shared --enable-static
  else
    SQLITE_CONFIGURE_FLAGS := --enable-shared --disable-static
  endif
else
  SQLITE_CONFIGURE_FLAGS := --disable-shared --enable-static
endif

$(SQLITE_STAMP):
	cd $(SQLITE_SRC) && ./configure $(SQLITE_CONFIGURE_FLAGS) && $(MAKE) libsqlite3.a
	@touch $@

.PHONY: sqlite
sqlite: $(SQLITE_STAMP)

# ---------------------------------------------------------------------------
# stumpless  (CMake)
# ---------------------------------------------------------------------------
STUMPLESS_SRC   := $(THIRDPARTY)/stumpless
STUMPLESS_BUILD := $(TPBUILD)/stumpless
STUMPLESS_STAMP := $(STUMPLESS_BUILD)/.built
ifeq ($(STUMPLESS_SHARED),YES)
  STUMPLESS_SHARED_FLAG := ON
else
  STUMPLESS_SHARED_FLAG := OFF
endif

$(STUMPLESS_STAMP):
	mkdir -p $(STUMPLESS_BUILD)
	$(call cmake_build,$(STUMPLESS_SRC),$(STUMPLESS_BUILD),\
	    -DBUILD_SHARED_LIBS=$(STUMPLESS_SHARED_FLAG) -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
	@touch $@

.PHONY: stumpless
stumpless: $(STUMPLESS_STAMP)

# ---------------------------------------------------------------------------
# cmocka  (CMake, out-of-source required)
# ---------------------------------------------------------------------------
CMOCKA_SRC   := $(THIRDPARTY)/cmocka
CMOCKA_BUILD := $(TPBUILD)/cmocka
CMOCKA_STAMP := $(CMOCKA_BUILD)/.built
ifeq ($(CMOCKA_SHARED),YES)
  CMOCKA_SHARED_FLAG := ON
else
  CMOCKA_SHARED_FLAG := OFF
endif

$(CMOCKA_STAMP):
	mkdir -p $(CMOCKA_BUILD)
	$(call cmake_build,$(CMOCKA_SRC),$(CMOCKA_BUILD),-DBUILD_SHARED_LIBS=$(CMOCKA_SHARED_FLAG))
	@touch $@

.PHONY: cmocka
cmocka: $(CMOCKA_STAMP)

# ---------------------------------------------------------------------------
# libconfig  (CMake)
# ---------------------------------------------------------------------------
LIBCONFIG_SRC   := $(THIRDPARTY)/libconfig
LIBCONFIG_BUILD := $(TPBUILD)/libconfig
LIBCONFIG_STAMP := $(LIBCONFIG_BUILD)/.built
ifeq ($(LIBCONFIG_SHARED),YES)
  LIBCONFIG_SHARED_FLAG := ON
else
  LIBCONFIG_SHARED_FLAG := OFF
endif

$(LIBCONFIG_STAMP):
	mkdir -p $(LIBCONFIG_BUILD)
	$(call cmake_build,$(LIBCONFIG_SRC),$(LIBCONFIG_BUILD),-DBUILD_SHARED_LIBS=$(LIBCONFIG_SHARED_FLAG))
	@touch $@

.PHONY: libconfig
libconfig: $(LIBCONFIG_STAMP)

# ---------------------------------------------------------------------------
# Aggregate third-party target
# ---------------------------------------------------------------------------
THIRDPARTY_STAMPS := \
    $(MBEDTLS_STAMP) \
    $(LIBUV_STAMP) \
    $(LIBSTROPHE_STAMP) \
    $(SQLITE_STAMP) \
    $(STUMPLESS_STAMP) \
    $(CMOCKA_STAMP) \
    $(LIBCONFIG_STAMP)

.PHONY: third-party
third-party: $(THIRDPARTY_STAMPS)

# ---------------------------------------------------------------------------
# Main target
# ---------------------------------------------------------------------------
.PHONY: all clean distclean test format help

TESTDIR     := tests
TEST_SRCS   := $(wildcard $(TESTDIR)/*.c)
TEST_BINS   := $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(TEST_SRCS))

# Objects shared between tests and the main binary (everything except main.o)
LIB_OBJS    := $(filter-out $(BUILDDIR)/main.o,$(OBJS))

TEST_LDFLAGS := \
    $(TPBUILD)/cmocka/libcmocka.a \
    $(TPBUILD)/libconfig/out/libconfig.a \
    $(TPBUILD)/stumpless/libstumpless.a \
    $(THIRDPARTY)/sqlite/libsqlite3.a \
    -lpthread -ldl -lm

all: $(THIRDPARTY_STAMPS) $(TARGET) $(COMPILE_COMMANDS)

$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(TP_LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/storage/%.o: $(SRCDIR)/storage/%.c | $(BUILDDIR)/storage
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/%: $(TESTDIR)/%.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(THIRDPARTY)/cmocka/include -o $@ $^ $(TEST_LDFLAGS)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/storage:
	mkdir -p $(BUILDDIR)/storage

# ---------------------------------------------------------------------------
# compile_commands.json generator
# ---------------------------------------------------------------------------
COMPILE_COMMANDS := $(BUILDDIR)/compile_commands.json

$(COMPILE_COMMANDS): $(OBJS) | $(BUILDDIR)
	@{ \
	  printf '[\n'; \
	  first=1; \
	  for src in $(SRCS); do \
	    obj=$(BUILDDIR)/$$(basename $$src .c).o; \
	    if [ -f "$$obj" ]; then \
	      if [ "$$first" -eq 1 ]; then first=0; else printf ',\n'; fi; \
	      printf '  {\n'; \
	      printf '    "directory": "%s",\n' "$$(pwd)"; \
	      printf '    "file": "%s",\n' "$$src"; \
	      printf '    "output": "%s",\n' "$$obj"; \
	      printf '    "command": "$(CC) $(CFLAGS) -c -o %s %s"\n' "$$obj" "$$src"; \
	      printf '  }'; \
	    fi; \
	  done; \
	  printf '\n]\n'; \
	} > $@

.PHONY: compile_commands
compile_commands: $(COMPILE_COMMANDS)

# ---------------------------------------------------------------------------
# Utility targets
# ---------------------------------------------------------------------------
clean:
	rm -rf build/

distclean: clean
	rm -f $(SQLITE_STAMP)

test: $(THIRDPARTY_STAMPS) $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "--- $$t ---"; $$t; done

format:
	@echo "Formatting source files..."
	clang-format -i $(SRCDIR)/*.c $(INCDIR)/*.h 2>/dev/null || true

help:
	@echo "Usage: make [TARGET] [OPTIONS]"
	@echo ""
	@echo "Targets:"
	@echo "  all          Build third-party libs + project (default)"
	@echo "  third-party  Build all third-party libraries"
	@echo "  mbedtls      Build mbedtls only"
	@echo "  libuv        Build libuv only"
	@echo "  libstrophe   Build libstrophe only"
	@echo "  sqlite       Build sqlite only"
	@echo "  stumpless    Build stumpless only"
	@echo "  cmocka       Build cmocka only"
	@echo "  libconfig    Build libconfig only"
	@echo "  clean        Remove build/ directory"
	@echo "  distclean    clean + remove sqlite in-source build"
	@echo "  test         Build and run tests"
	@echo "  format       Format source files with clang-format"
	@echo "  compile_commands  Generate compile_commands.json"
	@echo "  help         Show this help message"
	@echo ""
	@echo "Build types (BUILD=debug|release|asan, default: debug):"
	@echo "  debug        -g -O0"
	@echo "  release      -O2 -DNDEBUG"
	@echo "  asan         -g -O1 -fsanitize=address,undefined"
	@echo ""
	@echo "Per-library static/shared flags (YES/NO, defaults: STATIC=YES SHARED=NO):"
	@echo "  MBEDTLS_STATIC    MBEDTLS_SHARED"
	@echo "  LIBUV_STATIC      LIBUV_SHARED"
	@echo "  LIBSTROPHE_STATIC LIBSTROPHE_SHARED"
	@echo "  SQLITE_STATIC     SQLITE_SHARED"
	@echo "  STUMPLESS_STATIC  STUMPLESS_SHARED"
	@echo "  CMOCKA_STATIC     CMOCKA_SHARED"
	@echo "  LIBCONFIG_STATIC  LIBCONFIG_SHARED"
	@echo ""
	@echo "Example: make third-party LIBUV_SHARED=YES SQLITE_SHARED=YES"
	@echo "To force rebuild a library: rm build/debug/third_party/<lib>/.built"

-include $(DEPS)
