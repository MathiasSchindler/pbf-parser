CC := gcc-16
OS := linux
ARCH := x86_64
BUILD_DIR := build/freestanding-$(OS)-$(ARCH)
MAKE_JOBS ?= $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(MAKE_JOBS)
endif
MACOS_CC ?= clang
MACOS_ARCH := aarch64
MACOS_BUILD_DIR := build/freestanding-macos-arm64
MACOS_SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)

CFLAGS := -std=c11 -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fdata-sections -ffunction-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdinc -Isrc/shared -Isrc/shared/fontrender -Isrc/platform/linux -Isrc/platform/common -DNEWOS_DISABLE_STACK_GUARD_INIT -DFR_RASTER_DISABLE_SIMD
LDFLAGS := -nostdlib -static -no-pie -Wl,--gc-sections
MACOS_CFLAGS := -target arm64-apple-macos11 -std=c11 -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fdata-sections -ffunction-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdinc -Isrc/shared -Isrc/shared/fontrender -Isrc/platform/macos -Isrc/platform/common -Isrc/arch/aarch64/macos -DNEWOS_DISABLE_STACK_GUARD_INIT -DFR_RASTER_DISABLE_SIMD
MACOS_LDFLAGS := -nostdlib -Wl,-syslibroot,$(MACOS_SDKROOT) -Wl,-e,_start -Wl,-dead_strip -lSystem

RUNTIME_SRCS := \
    src/arch/x86_64/linux/crt0.S \
    src/arch/x86_64/linux/syscall_stubs.S \
    src/platform/linux/io.c \
    src/shared/runtime/io.c \
    src/shared/runtime/memory.c \
    src/shared/runtime/parse.c \
    src/shared/runtime/string.c

FONTRENDER_SRCS := \
    src/shared/fontrender/fr_platform.c \
    src/shared/fontrender/fr_ttf.c \
    src/shared/fontrender/fr_raster.c \
    src/shared/fontrender/font_backend_truetype.c \
    src/shared/fontrender_runtime.c

MACOS_RUNTIME_SRCS := \
    src/arch/aarch64/macos/crt0.S \
    src/platform/macos/io.c \
    src/platform/macos/thread.c \
    src/platform/macos/time.c \
    src/shared/runtime/io.c \
    src/shared/runtime/memory.c \
    src/shared/runtime/parse.c \
    src/shared/runtime/string.c

MACOS_OSMRENDERPACKV2_SRCS := \
    $(MACOS_RUNTIME_SRCS) \
    src/shared/compression/zlib.c \
    src/shared/pbf.c \
    src/shared/osmrpack.c \
    src/tools/osmrenderpackv2.c

MACOS_OSMRPACKINFO_SRCS := \
    $(MACOS_RUNTIME_SRCS) \
    src/shared/osmrpack.c \
    src/tools/osmrpackinfo.c

MACOS_OSMRENDER_RPACK_SRCS := \
    $(MACOS_RUNTIME_SRCS) \
    src/shared/compression/crc32.c \
    src/shared/compression/zlib.c \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/shared/osmrpack.c \
    src/tools/osmrender_rpack.c

MACOS_OSMWALKROUTE_SRCS := \
    $(MACOS_RUNTIME_SRCS) \
    src/shared/compression/zlib.c \
    src/shared/pbf.c \
    src/tools/osmwalkroute.c

MACOS_OSMROUTEPACK_SRCS := \
    $(MACOS_RUNTIME_SRCS) \
    src/shared/compression/zlib.c \
    src/shared/pbf.c \
    src/tools/osmroutepack.c

MACOS_OSMRTEINFO_SRCS := \
    $(MACOS_RUNTIME_SRCS) \
    src/tools/osmrteinfo.c

MACOS_RTEWALKROUTE_SRCS := \
    $(MACOS_RUNTIME_SRCS) \
    src/tools/rtewalkroute.c

MACOS_THREADTEST_SRCS := \
    $(MACOS_RUNTIME_SRCS) \
    src/tools/threadtest.c

PBFINFO_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/thread.c \
    src/shared/compression/zlib.c \
    src/shared/pbf.c \
    src/tools/pbfinfo.c

OSMLOOKUP_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/shared/compression/zlib.c \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/tools/osmlookup.c

OSMNODEINDEX_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/shared/compression/zlib.c \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/tools/osmnodeindex.c

OSMWAYINDEX_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/shared/compression/zlib.c \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/tools/osmwayindex.c

OSMINDEX_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/shared/compression/zlib.c \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/tools/osmindex.c

OSMRELINDEX_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/shared/compression/zlib.c \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/tools/osmrelindex.c

OSMSPINDEX_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/shared/compression/zlib.c \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/tools/osmspindex.c

OSMADDRESSES_SRCS := \
    $(RUNTIME_SRCS) \
    src/shared/compression/zlib.c \
    src/shared/pbf.c \
    src/tools/osmaddresses.c

OSMRENDER_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/platform/linux/time.c \
    src/shared/compression/crc32.c \
    src/shared/compression/zlib.c \
    src/shared/runtime/unicode_utf8.c \
    $(FONTRENDER_SRCS) \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/shared/simple_config.c \
    src/tools/osmrender.c

OSMRENDERPACKV2_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/platform/linux/thread.c \
    src/platform/linux/time.c \
    src/shared/compression/zlib.c \
    src/shared/pbf.c \
    src/shared/osmrpack.c \
    src/tools/osmrenderpackv2.c

OSMRPACKINFO_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/shared/osmrpack.c \
    src/tools/osmrpackinfo.c

OSMRENDER_RPACK_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/platform/linux/time.c \
    src/shared/compression/crc32.c \
    src/shared/compression/zlib.c \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/shared/osmrpack.c \
    src/tools/osmrender_rpack.c

OSMWALKROUTE_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/thread.c \
    src/shared/compression/zlib.c \
    src/shared/pbf.c \
    src/tools/osmwalkroute.c

OSMROUTEPACK_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/thread.c \
    src/platform/linux/time.c \
    src/shared/compression/zlib.c \
    src/shared/pbf.c \
    src/tools/osmroutepack.c

OSMRTEINFO_SRCS := \
    $(RUNTIME_SRCS) \
    src/tools/osmrteinfo.c

RTEWALKROUTE_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/time.c \
    src/tools/rtewalkroute.c

THREADTEST_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/thread.c \
    src/tools/threadtest.c

FONTTEST_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    $(FONTRENDER_SRCS) \
    src/tools/fonttest.c

.PHONY: all clean threadtest macos-rpack-tools macos-threadtest

all: $(BUILD_DIR)/pbfinfo $(BUILD_DIR)/osmlookup $(BUILD_DIR)/osmnodeindex $(BUILD_DIR)/osmwayindex $(BUILD_DIR)/osmindex $(BUILD_DIR)/osmrelindex $(BUILD_DIR)/osmspindex $(BUILD_DIR)/osmaddresses $(BUILD_DIR)/osmrender $(BUILD_DIR)/osmrenderpackv2 $(BUILD_DIR)/osmrpackinfo $(BUILD_DIR)/osmrender-rpack $(BUILD_DIR)/osmwalkroute $(BUILD_DIR)/osmroutepack $(BUILD_DIR)/osmrteinfo $(BUILD_DIR)/rtewalkroute $(BUILD_DIR)/threadtest $(BUILD_DIR)/fonttest

threadtest: $(BUILD_DIR)/threadtest

macos-rpack-tools: $(MACOS_BUILD_DIR)/osmrenderpackv2 $(MACOS_BUILD_DIR)/osmrpackinfo $(MACOS_BUILD_DIR)/osmrender-rpack $(MACOS_BUILD_DIR)/osmwalkroute $(MACOS_BUILD_DIR)/osmroutepack $(MACOS_BUILD_DIR)/osmrteinfo $(MACOS_BUILD_DIR)/rtewalkroute

macos-threadtest: $(MACOS_BUILD_DIR)/threadtest

$(BUILD_DIR):
	mkdir -p $@

$(MACOS_BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/pbfinfo: $(PBFINFO_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(PBFINFO_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmlookup: $(OSMLOOKUP_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMLOOKUP_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmnodeindex: $(OSMNODEINDEX_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMNODEINDEX_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmwayindex: $(OSMWAYINDEX_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMWAYINDEX_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmindex: $(OSMINDEX_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMINDEX_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmrelindex: $(OSMRELINDEX_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRELINDEX_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmspindex: $(OSMSPINDEX_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMSPINDEX_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmaddresses: $(OSMADDRESSES_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMADDRESSES_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmrender: $(OSMRENDER_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRENDER_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmrenderpackv2: $(OSMRENDERPACKV2_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRENDERPACKV2_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmrpackinfo: $(OSMRPACKINFO_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRPACKINFO_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmrender-rpack: $(OSMRENDER_RPACK_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRENDER_RPACK_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmwalkroute: $(OSMWALKROUTE_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMWALKROUTE_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmroutepack: $(OSMROUTEPACK_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMROUTEPACK_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmrteinfo: $(OSMRTEINFO_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRTEINFO_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/rtewalkroute: $(RTEWALKROUTE_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(RTEWALKROUTE_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/threadtest: $(THREADTEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREADTEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/fonttest: $(FONTTEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(FONTTEST_SRCS) $(LDFLAGS) -o $@

$(MACOS_BUILD_DIR)/osmrenderpackv2: $(MACOS_OSMRENDERPACKV2_SRCS) | $(MACOS_BUILD_DIR)
	$(MACOS_CC) $(MACOS_CFLAGS) $(MACOS_OSMRENDERPACKV2_SRCS) $(MACOS_LDFLAGS) -o $@

$(MACOS_BUILD_DIR)/osmrpackinfo: $(MACOS_OSMRPACKINFO_SRCS) | $(MACOS_BUILD_DIR)
	$(MACOS_CC) $(MACOS_CFLAGS) $(MACOS_OSMRPACKINFO_SRCS) $(MACOS_LDFLAGS) -o $@

$(MACOS_BUILD_DIR)/osmrender-rpack: $(MACOS_OSMRENDER_RPACK_SRCS) | $(MACOS_BUILD_DIR)
	$(MACOS_CC) $(MACOS_CFLAGS) $(MACOS_OSMRENDER_RPACK_SRCS) $(MACOS_LDFLAGS) -o $@

$(MACOS_BUILD_DIR)/osmwalkroute: $(MACOS_OSMWALKROUTE_SRCS) | $(MACOS_BUILD_DIR)
	$(MACOS_CC) $(MACOS_CFLAGS) $(MACOS_OSMWALKROUTE_SRCS) $(MACOS_LDFLAGS) -o $@

$(MACOS_BUILD_DIR)/osmroutepack: $(MACOS_OSMROUTEPACK_SRCS) | $(MACOS_BUILD_DIR)
	$(MACOS_CC) $(MACOS_CFLAGS) $(MACOS_OSMROUTEPACK_SRCS) $(MACOS_LDFLAGS) -o $@

$(MACOS_BUILD_DIR)/osmrteinfo: $(MACOS_OSMRTEINFO_SRCS) | $(MACOS_BUILD_DIR)
	$(MACOS_CC) $(MACOS_CFLAGS) $(MACOS_OSMRTEINFO_SRCS) $(MACOS_LDFLAGS) -o $@

$(MACOS_BUILD_DIR)/rtewalkroute: $(MACOS_RTEWALKROUTE_SRCS) | $(MACOS_BUILD_DIR)
	$(MACOS_CC) $(MACOS_CFLAGS) $(MACOS_RTEWALKROUTE_SRCS) $(MACOS_LDFLAGS) -o $@

$(MACOS_BUILD_DIR)/threadtest: $(MACOS_THREADTEST_SRCS) | $(MACOS_BUILD_DIR)
	$(MACOS_CC) $(MACOS_CFLAGS) $(MACOS_THREADTEST_SRCS) $(MACOS_LDFLAGS) -o $@

clean:
	rm -rf build
