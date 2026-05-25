CC := gcc-16
OS := linux
ARCH := x86_64
BUILD_DIR := build/freestanding-$(OS)-$(ARCH)

CFLAGS := -std=c11 -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fdata-sections -ffunction-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdinc -Isrc/shared -Isrc/platform/linux -DNEWOS_DISABLE_STACK_GUARD_INIT
LDFLAGS := -nostdlib -static -no-pie -Wl,--gc-sections

RUNTIME_SRCS := \
    src/arch/x86_64/linux/crt0.S \
    src/arch/x86_64/linux/syscall_stubs.S \
    src/platform/linux/io.c \
    src/shared/runtime/io.c \
    src/shared/runtime/memory.c \
    src/shared/runtime/parse.c \
    src/shared/runtime/string.c

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

OSMADDRESSES_SRCS := \
    $(RUNTIME_SRCS) \
    src/shared/compression/zlib.c \
    src/shared/pbf.c \
    src/tools/osmaddresses.c

OSMRENDER_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/fs.c \
    src/shared/compression/crc32.c \
    src/shared/compression/zlib.c \
    src/shared/osm_index.c \
    src/shared/pbf.c \
    src/shared/simple_config.c \
    src/tools/osmrender.c

THREADTEST_SRCS := \
    $(RUNTIME_SRCS) \
    src/platform/linux/thread.c \
    src/tools/threadtest.c

.PHONY: all clean threadtest

all: $(BUILD_DIR)/pbfinfo $(BUILD_DIR)/osmlookup $(BUILD_DIR)/osmnodeindex $(BUILD_DIR)/osmwayindex $(BUILD_DIR)/osmaddresses $(BUILD_DIR)/osmrender $(BUILD_DIR)/threadtest

threadtest: $(BUILD_DIR)/threadtest

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/pbfinfo: $(PBFINFO_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(PBFINFO_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmlookup: $(OSMLOOKUP_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMLOOKUP_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmnodeindex: $(OSMNODEINDEX_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMNODEINDEX_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmwayindex: $(OSMWAYINDEX_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMWAYINDEX_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmaddresses: $(OSMADDRESSES_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMADDRESSES_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmrender: $(OSMRENDER_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRENDER_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/threadtest: $(THREADTEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREADTEST_SRCS) $(LDFLAGS) -o $@

clean:
	rm -rf build