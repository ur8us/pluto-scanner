SHELL := /bin/sh

PROGRAM := pluto-scanner
TEST_PROGRAM := pluto-scanner-cic-test
BUILD_DIR ?= .build

UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)
UNAME_M := $(shell uname -m 2>/dev/null || echo unknown)

PKG_CONFIG ?= pkg-config
CC ?= cc

CFLAGS ?= -O2
WARN_CFLAGS ?= -Wall -Wextra -Wformat-security
CPPFLAGS ?=
LDFLAGS ?=
STD_CFLAGS ?= -std=c99

EXEEXT :=
THREAD_CFLAGS := -pthread
THREAD_LIBS := -pthread
MATH_LIBS := -lm
OS_LIBS :=
HOST_TARGET := linux-$(UNAME_M)
STATIC_MODE ?= full
VERIFY_STATIC ?= 0

ifneq (,$(filter Darwin,$(UNAME_S)))
  HOST_TARGET := macos-$(UNAME_M)
  THREAD_CFLAGS :=
  THREAD_LIBS := -lpthread
  STATIC_MODE := deps
endif

ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
  HOST_TARGET := windows-x86_64
  EXEEXT := .exe
  OS_LIBS := -lws2_32 -liphlpapi
endif

ifeq ($(UNAME_M),x86_64)
  ifneq (,$(filter Linux,$(UNAME_S)))
    HOST_TARGET := linux-x86_64
  endif
endif

ifeq ($(UNAME_M),aarch64)
  ifneq (,$(filter Linux,$(UNAME_S)))
    HOST_TARGET := linux-aarch64
  endif
endif

IIO_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libiio 2>/dev/null)
IIO_LIBS ?= $(shell $(PKG_CONFIG) --libs libiio 2>/dev/null || echo -liio)
ifeq ($(strip $(IIO_LIBS)),)
  IIO_LIBS := -liio
endif

CPPFLAGS += $(IIO_CFLAGS)
LDLIBS ?= $(IIO_LIBS) $(MATH_LIBS) $(THREAD_LIBS) $(OS_LIBS)

TARGET := $(PROGRAM)$(EXEEXT)
TEST_TARGET := $(BUILD_DIR)/tests/$(TEST_PROGRAM)$(EXEEXT)

ifeq ($(EXEEXT),.exe)
  COMPAT_TARGETS := $(PROGRAM)
  COMPAT_TEST_TARGETS :=
else
  COMPAT_TARGETS :=
  COMPAT_TEST_TARGETS :=
endif

RELEASE_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
RELEASE_TARGET ?= $(HOST_TARGET)
RELEASE_DIR ?= $(CURDIR)/.release
DEPS_PREFIX ?= $(RELEASE_DIR)/deps
BUILD_ROOT ?= $(RELEASE_DIR)/build-$(RELEASE_TARGET)
OUT_DIR ?= $(RELEASE_DIR)/out
RELEASE_BINARY ?= $(RELEASE_DIR)/bin/$(TARGET)

.PHONY: all help build-info clean distclean run check ci-check smoke-test cic-synthetic-test
.PHONY: install-deps-help release-deps release-binary package-tar package-zip
.PHONY: package-appimage package-dmg release-local

all: $(TARGET) $(COMPAT_TARGETS)

$(TARGET): main.c
	$(CC) $(STD_CFLAGS) $(WARN_CFLAGS) $(CFLAGS) $(THREAD_CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/tests:
	mkdir -p $@

$(TEST_TARGET): main.c | $(BUILD_DIR)/tests
	$(CC) $(STD_CFLAGS) $(WARN_CFLAGS) -Wno-unused-function $(CFLAGS) $(THREAD_CFLAGS) $(CPPFLAGS) -DPSEUDO_RANDOM_SAMPLE_SOURCE=2 -o $@ $< $(LDFLAGS) $(LDLIBS)

ifeq ($(EXEEXT),.exe)
$(PROGRAM): $(TARGET)
	cp -f $< $@

endif

help:
	@echo "Pluto SDR Scanner build targets"
	@echo ""
	@echo "Local source build:"
	@echo "  make                  Build $(TARGET) for this host"
	@echo "  make all              Same as make"
	@echo "  make run              Run with default PLUTO_URI=ip:192.168.2.1"
	@echo "  make check            Build and run local validation checks"
	@echo "  make ci-check         Run GitHub-safe checks without developer test apps"
	@echo "  make clean            Remove local build outputs"
	@echo "  make cic-synthetic-test"
	@echo ""
	@echo "Diagnostics:"
	@echo "  make build-info       Show detected OS, compiler flags, and libraries"
	@echo "  make install-deps-help"
	@echo ""
	@echo "Local release packaging, using tools/ci scripts:"
	@echo "  make release-deps     Build static libxml2/libiio into .release/deps"
	@echo "  make release-binary   Build .release/bin/$(TARGET)"
	@echo "  make package-tar      Build tar.gz package"
	@echo "  make package-zip      Build zip package"
	@echo "  make package-appimage Build AppImage package on Linux"
	@echo "  make package-dmg      Build dmg package on macOS"
	@echo "  make release-local    Build the package type for this host"

build-info:
	@echo "UNAME_S=$(UNAME_S)"
	@echo "UNAME_M=$(UNAME_M)"
	@echo "HOST_TARGET=$(HOST_TARGET)"
	@echo "TARGET=$(TARGET)"
	@echo "CC=$(CC)"
	@echo "STD_CFLAGS=$(STD_CFLAGS)"
	@echo "WARN_CFLAGS=$(WARN_CFLAGS)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "THREAD_CFLAGS=$(THREAD_CFLAGS)"
	@echo "CPPFLAGS=$(CPPFLAGS)"
	@echo "LDFLAGS=$(LDFLAGS)"
	@echo "LDLIBS=$(LDLIBS)"
	@echo "PKG_CONFIG=$(PKG_CONFIG)"
	@echo "IIO_CFLAGS=$(IIO_CFLAGS)"
	@echo "IIO_LIBS=$(IIO_LIBS)"

install-deps-help:
	@echo "Linux Debian/Ubuntu:"
	@echo "  sudo apt install build-essential libiio-dev nodejs python3 python3-pip"
	@echo "  python3 -m pip install -r requirements.txt"
	@echo ""
	@echo "macOS with Homebrew:"
	@echo "  xcode-select --install"
	@echo "  brew install libiio node python pkg-config"
	@echo "  make"
	@echo ""
	@echo "Windows MSYS2 UCRT64:"
	@echo "  pacman -S --needed base-devel git make \\"
	@echo "    mingw-w64-ucrt-x86_64-gcc \\"
	@echo "    mingw-w64-ucrt-x86_64-libiio \\"
	@echo "    mingw-w64-ucrt-x86_64-pkgconf \\"
	@echo "    mingw-w64-ucrt-x86_64-python \\"
	@echo "    mingw-w64-ucrt-x86_64-nodejs"
	@echo "  make"

run: $(TARGET)
	PLUTO_URI="$${PLUTO_URI:-ip:192.168.2.1}" ./$(TARGET)

smoke-test:
	tools/http_smoke_test.sh

check: all $(TEST_TARGET)
	perl -0777 -ne 'print $$1 if /<script>(.*?)<\/script>/s' index.html | node --check
	tools/cic_stability_check.py --quiet
	tools/cic_continuity_check.py --quiet
	PLUTO_CIC_TEST_BINARY="$(TEST_TARGET)" tools/cic_synthetic_signal_check.py --quiet
	PLUTO_CIC_TEST_BINARY="$(TEST_TARGET)" tools/min_rate_overlap_check.py >/dev/null
	PLUTO_CIC_TEST_BINARY="$(TEST_TARGET)" tools/cached_preview_check.py >/dev/null
	PLUTO_CIC_TEST_BINARY="$(TEST_TARGET)" PLUTO_TEST_DISPLAY_BINS=1346 tools/cached_preview_check.py >/dev/null
	PLUTO_CIC_TEST_BINARY="$(TEST_TARGET)" tools/frequency_coordinate_check.py >/dev/null
	PLUTO_CIC_TEST_BINARY="$(TEST_TARGET)" tools/startup_resume_check.py >/dev/null
	@echo "Build checks passed."

ci-check: all
	perl -0777 -ne 'print $$1 if /<script>(.*?)<\/script>/s' index.html | node --check
	tools/cic_stability_check.py --quiet
	tools/cic_continuity_check.py --quiet
	@echo "CI checks passed."

cic-synthetic-test: $(TEST_TARGET)
	PLUTO_CIC_TEST_BINARY="$(TEST_TARGET)" tools/cic_synthetic_signal_check.py

release-deps:
	DEPS_PREFIX="$(DEPS_PREFIX)" BUILD_ROOT="$(BUILD_ROOT)" tools/ci/build-static-deps.sh

release-binary: release-deps
	STATIC_MODE="$(STATIC_MODE)" VERIFY_STATIC="$(VERIFY_STATIC)" DEPS_PREFIX="$(DEPS_PREFIX)" EXE_EXT="$(EXEEXT)" OUT="$(RELEASE_BINARY)" tools/ci/build-release-binary.sh

package-tar: release-binary
	OUT_DIR="$(OUT_DIR)" tools/ci/package-release.sh "$(RELEASE_VERSION)" "$(RELEASE_TARGET)" "$(RELEASE_BINARY)" tar.gz

package-zip: release-binary
	OUT_DIR="$(OUT_DIR)" tools/ci/package-release.sh "$(RELEASE_VERSION)" "$(RELEASE_TARGET)" "$(RELEASE_BINARY)" zip

package-appimage: release-binary
	OUT_DIR="$(OUT_DIR)" tools/ci/package-appimage.sh "$(RELEASE_VERSION)" "$(RELEASE_TARGET)" "$(RELEASE_BINARY)" "$(if $(filter aarch64,$(UNAME_M)),aarch64,x86_64)"

package-dmg: release-binary
	OUT_DIR="$(OUT_DIR)" tools/ci/package-macos-dmg.sh "$(RELEASE_VERSION)" "$(RELEASE_BINARY)"

ifneq (,$(filter Darwin,$(UNAME_S)))
release-local: package-dmg
else ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
release-local: package-zip
else
release-local: package-tar package-appimage
endif

clean:
	rm -f $(TARGET) $(COMPAT_TARGETS)
	rm -rf "$(BUILD_DIR)"

distclean: clean
	rm -rf "$(RELEASE_DIR)"
