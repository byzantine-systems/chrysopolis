# Chrysopolis reference-stack build.
#
# Drives the LionsOS POSIX libc + sDDF serial/timer drivers under a single
# make, exactly the way the upstream examples/*/*.mk snippets do (this is a
# trimmed examples/posix_test/posix_test.mk). Consumed by the `lionsStack`
# Nix derivation, which supplies a hermetic toolchain (clang/lld/llvm + dtc),
# the Microkit SDK, and the reconstructed LionsOS source tree (lionsosSrc)
# with its dep/sddf and dep/musllibc submodules populated from flake inputs.
#
# Required variables (set by the derivation):
#   LIONSOS         reconstructed LionsOS source tree (submodules populated)
#   MICROKIT_SDK    Microkit SDK root (provides the board dir + libmicrokit)
#   MICROKIT_BOARD  e.g. qemu_virt_aarch64
#   MICROKIT_CONFIG e.g. debug
#
# Produces, in $(BUILD_DIR): libc/lib/libc.a, libc/include/, and the board DTB.
# The sDDF driver/virtualiser PDs and libmicrokitco are built separately by Zig
# packages (tools/sddf-drivers, libmicrokitco), their .mk includes below remain
# only so their rules/vars parse; their targets are no longer requested by `all`.

MICROKIT_CONFIG ?= debug
TOOLCHAIN := clang
SUPPORTED_BOARDS := qemu_virt_aarch64 maaxboard

SDDF := $(LIONSOS)/dep/sddf
BUILD_DIR := $(CURDIR)

DRIVER_IMAGES := \
	timer_driver.elf \
	serial_driver.elf \
	serial_virt_tx.elf \
	serial_virt_rx.elf

# The `all` rule is defined at the bottom, after the includes that set
# LIONS_LIBC and DTB; declare it the default goal here so it wins over any
# first target the included snippets introduce.
.DEFAULT_GOAL := all
.PHONY: all

# common.mk reads ARCH from $(MICROKIT_SDK)/board/.../include/kernel and
# pulls in the board snippet (driver dirs, CPU) + the clang toolchain
# (CC/LD/AR/RANLIB/OBJCOPY, TARGET=aarch64-none-elf, CFLAGS_ARCH). It also
# defines the $(DTB): $(DTS) rule that compiles the board device tree.
include $(SDDF)/tools/make/board/common.mk

# Match the example CFLAGS so the POSIX layer + drivers see the LionsOS and
# sDDF headers (and the microkit shim) they include.
CFLAGS += \
	-Wno-bitwise-op-parentheses \
	-Wno-shift-op-parentheses \
	-Wno-unused-function \
	-Wno-tautological-constant-out-of-range-compare \
	-I$(LIONSOS)/include \
	-I$(SDDF)/include \
	-I$(SDDF)/include/microkit \
	-DMAX_FDS=1024

# Builds libc/lib/libc.a (musl + posix/*.c + compiler_rt + fs helpers) and
# installs the headers into libc/include. Defines LIONS_LIBC.
include $(LIONSOS)/lib/libc/libc.mk

LDFLAGS := -L$(BOARD_DIR)/lib -L$(LIONS_LIBC)/lib
LIBS := -lmicrokit -Tmicrokit.ld libsddf_util_debug.a -lc

# The drivers and util library compile against the LionsOS libc headers.
SDDF_LIBC_INCLUDE := $(LIONS_LIBC)/include
include $(SDDF)/util/util.mk
include $(SDDF)/drivers/timer/$(TIMER_DRIV_DIR)/timer_driver.mk
include $(SDDF)/drivers/serial/$(UART_DRIV_DIR)/serial_driver.mk
include $(SDDF)/serial/components/serial_components.mk

# Block subsystem: the virtio-mmio block driver + virtualiser back the FAT
# filesystem server (the disk ERTS loads its boot script / modules from).
include $(SDDF)/drivers/blk/$(BLK_DRIV_DIR)/blk_driver.mk
include $(SDDF)/blk/components/blk_components.mk

# FAT filesystem server (fat.elf): FatFS (dep/ff15) + lib_fs_server + a fat
# variant of libmicrokitco, linked against the LionsOS libc. fat.mk also sets
# LIBMICROKITCO_CFLAGS_fat (consumed by the libmicrokitco include below) and
# includes lib_fs_server.mk.
LIBMICROKITCO_PATH := $(LIONSOS)/dep/libmicrokitco
FAT_LIBC_INCLUDE := $(LIONS_LIBC)/include
FAT_LIBC_LIB := $(LIONS_LIBC)/lib/libc.a
include $(LIONSOS)/components/fs/fat/fat.mk

# libmicrokitco: the cooperative cothread library the ERTS threading layer
# and the fat fs_server run on. Both variants are NAMED (_beam, _fat), the 
# VARIANTS mechanism drops a bare/empty-suffix variant
# when a named one is present. _beam uses libmicrokitco_opts.h in this build
# dir; _fat uses the fat component's own (fat.mk's FAT_CFLAGS).
LIBMICROKITCO_LIBC_INCLUDE := $(LIONS_LIBC)/include
LIBMICROKITCO_CFLAGS_beam := -I$(CURDIR)
include $(LIBMICROKITCO_PATH)/libmicrokitco.mk

FS_IMAGES := blk_driver.elf blk_virt.elf fat.elf

# Every ELF needs libc.a and the debug util archive present first.
$(DRIVER_IMAGES) $(FS_IMAGES): $(LIONS_LIBC)/lib/libc.a libsddf_util_debug.a

# Defined here, after the includes, so LIONS_LIBC and DTB are non-empty.
# The sDDF driver/virtualiser PDs ($(DRIVER_IMAGES)/$(FS_IMAGES)) and libmicrokitco
# are built by Zig now (tools/sddf-drivers, libmicrokitco), so they are intentionally 
# NOT in `all`; this make only produces the musl libc.a (autotools) and the 
# board DTB.
all: $(LIONS_LIBC)/lib/libc.a $(DTB)
