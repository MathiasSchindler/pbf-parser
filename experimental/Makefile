CC := gcc-16
OS := linux
ARCH := x86_64
BUILD_DIR := build/freestanding-$(OS)-$(ARCH)
ROOT := ..

CFLAGS := -std=c11 -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fdata-sections -ffunction-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdinc -I$(ROOT)/src/shared -I$(ROOT)/src/platform/linux -I$(ROOT)/src/platform/common -I./src -DNEWOS_DISABLE_STACK_GUARD_INIT
LDFLAGS := -nostdlib -static -no-pie -Wl,--gc-sections

RUNTIME_SRCS := \
    $(ROOT)/src/arch/x86_64/linux/crt0.S \
    $(ROOT)/src/arch/x86_64/linux/syscall_stubs.S \
    $(ROOT)/src/platform/linux/io.c \
    $(ROOT)/src/platform/linux/time.c \
    $(ROOT)/src/shared/runtime/io.c \
    $(ROOT)/src/shared/runtime/memory.c \
    $(ROOT)/src/shared/runtime/parse.c \
    $(ROOT)/src/shared/runtime/string.c

PACK_COMMON_SRCS := \
    $(RUNTIME_SRCS) \
    $(ROOT)/src/platform/linux/fs.c \
    $(ROOT)/src/shared/compression/crc32.c \
    $(ROOT)/src/shared/compression/zlib.c \
    $(ROOT)/src/shared/pbf.c \
    src/osmrpack.c

OSMRENDERPACK_SRCS := \
    $(PACK_COMMON_SRCS) \
    src/osmrenderpack.c

OSMRPACKINFO_SRCS := \
    $(PACK_COMMON_SRCS) \
    src/osmrpackinfo.c

OSMRENDERPACKRENDER_SRCS := \
    $(PACK_COMMON_SRCS) \
    $(ROOT)/src/shared/osm_index.c \
    src/osmrenderpackrender.c

.PHONY: all clean

all: $(BUILD_DIR)/osmrenderpack $(BUILD_DIR)/osmrpackinfo $(BUILD_DIR)/osmrenderpackrender

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/osmrenderpack: $(OSMRENDERPACK_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRENDERPACK_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmrpackinfo: $(OSMRPACKINFO_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRPACKINFO_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/osmrenderpackrender: $(OSMRENDERPACKRENDER_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OSMRENDERPACKRENDER_SRCS) $(LDFLAGS) -o $@

clean:
	rm -rf build
