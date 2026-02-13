# PageFault Claude - Bare-metal Claude client via x86 page faults
#
# The MMU is the computer. Zero instructions executed.

# Tools
AS      = gcc
CC      = gcc
LD      = ld
NASM    = nasm
GRUB    = grub-mkrescue
QEMU    = qemu-system-i386
QEMUFLAGS = -device isa-debug-exit,iobase=0x501,iosize=0x04 -no-reboot

# Flags - build for 32-bit bare metal
CFLAGS  = -m32 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
          -Wall -Wextra -O2 -g -Ikernel
ASFLAGS = -m32 -c
LDFLAGS = -m elf_i386 -T kernel/linker.ld -nostdlib

# Output
KERNEL  = build/pagefault_claude
ISO     = build/pagefault_claude.iso

# Linker script
LINKER  = kernel/linker.ld

# Objects
OBJS = build/boot.o build/kernel.o build/weirdmachine.o build/set_gdtr.o

.PHONY: all clean run run-serial deps iso run-proxy run-headless

all: $(KERNEL)

# Assembly objects
build/boot.o: kernel/boot.S | build
	$(AS) $(ASFLAGS) $< -o $@

build/set_gdtr.o: kernel/set_gdtr.S | build
	$(AS) $(ASFLAGS) $< -o $@

# C objects
build/kernel.o: kernel/kernel.c kernel/weirdmachine.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/weirdmachine.o: kernel/weirdmachine.c kernel/weirdmachine.h | build
	$(CC) $(CFLAGS) -c $< -o $@

# Link
$(KERNEL): $(OBJS) $(LINKER)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# Create bootable ISO via GRUB
iso: $(ISO)

$(ISO): $(KERNEL) kernel/grub.cfg | build
	@mkdir -p build/isodir/boot/grub
	cp $(KERNEL) build/isodir/boot/pagefault_claude
	cp kernel/grub.cfg build/isodir/boot/grub/grub.cfg
	$(GRUB) -o $@ build/isodir 2>/dev/null

build:
	@mkdir -p build

# Run in QEMU (VGA display)
run: $(KERNEL)
	$(QEMU) $(QEMUFLAGS) -kernel $< -display curses

# Run with serial on stdio
run-serial: $(KERNEL)
	$(QEMU) $(QEMUFLAGS) -kernel $< -display curses -serial stdio

# Run headless (serial on stdio, no display)
run-headless: $(KERNEL)
	$(QEMU) $(QEMUFLAGS) -kernel $< -nographic

# Run headless with extra RAM for page fault weird machine
run-wm: $(KERNEL)
	$(QEMU) $(QEMUFLAGS) -kernel $< -nographic -m 2048

# Run with the host proxy (type in the QEMU window)
# Proxy logs go to proxy.log to avoid corrupting the curses display.
run-proxy: $(KERNEL)
	@echo "Starting Claude proxy in background (logging to proxy.log)..."
	python3 proxy/claude_proxy.py --port 4321 >proxy.log 2>&1 &
	@sleep 1
	@echo "Starting QEMU — type directly in this window..."
	$(QEMU) $(QEMUFLAGS) -kernel $< -display curses -m 2048 \
		-serial tcp:127.0.0.1:4321,server=on,wait=on

# Install build dependencies (Ubuntu/Debian)
deps:
	@echo "Installing build dependencies..."
	apt-get update -qq
	apt-get install -y -qq nasm gcc qemu-system-x86 grub-pc-bin xorriso mtools python3-pip
	pip3 install anthropic pyserial 2>/dev/null || true
	@echo "Dependencies installed."

clean:
	rm -rf build
