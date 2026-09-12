# ======================================================================
# Toolchain
# ======================================================================
CROSS_PREFIX ?= x86_64-elf-
CC       = $(CROSS_PREFIX)gcc
CXX      = $(CROSS_PREFIX)g++
LD       = $(CROSS_PREFIX)ld
NASM     = nasm
OBJCOPY  = $(CROSS_PREFIX)objcopy
NM       = $(CROSS_PREFIX)nm
HOSTCC   ?= gcc
QEMU     ?= qemu-system-x86_64

# Git Bash runs recipes through sh.exe; a normal Windows prompt uses cmd.exe.
ifeq ($(OS),Windows_NT)
	POSIX_SHELL := $(MSYSTEM)
else
	POSIX_SHELL := 1
endif
ifeq ($(POSIX_SHELL),)
	DD = C:\msys64\usr\bin\dd.exe
	DU = C:\msys64\usr\bin\du.exe
define MAKE_DIR
@if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
endef
else
	DD = dd
	DU = du
define MAKE_DIR
@mkdir -p "$(1)"
endef
endif

# ======================================================================
# Flags
# ======================================================================
CFLAGS   = -ffreestanding -O2 -Wall -Wextra -m32 -nostdlib -nostartfiles \
           -fno-pie -I . -I kernel/arch/x86/include
CXXFLAGS = -ffreestanding -O2 -Wall -Wextra -m32 -nostdlib -nostartfiles \
           -fno-pie -fno-rtti -fno-exceptions -I . -I kernel/arch/x86/include
LDFLAGS  = -T linker.ld -m elf_i386

APP_LOAD_ADDR = 0x200000
APPFLAGS = -ffreestanding -O2 -m32 -nostdlib -nostartfiles \
           -fno-pie -fno-stack-protector -I . -I kernel/arch/x86/include

# ======================================================================
# Directories
# ======================================================================
BOOT_DIR    = boot
KERNEL_DIR  = kernel
SHELL_DIR   = shell
FILESYS_DIR = filesys
APPS_DIR    = apps
WM_DIR      = wm
TOOLS_DIR   = tools
BUILD_DIR   = build
HOSTTEST_DIR = hosttest

# ======================================================================
# Sources
# ======================================================================
# kernel/ is now nested (kernel/arch/x86, kernel/mem, kernel/proc,
# kernel/ui, kernel/drivers/...), so a plain $(wildcard DIR/*.c) would
# miss everything below the top level. rwildcard recurses into
# subdirectories; $1 is the directory (with trailing /), $2 the pattern.
rwildcard = $(wildcard $1$2) $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2))

C_SRCS   := $(call rwildcard,$(KERNEL_DIR)/,*.c) $(wildcard $(SHELL_DIR)/*.c $(FILESYS_DIR)/*.c)
CPP_SRCS := $(call rwildcard,$(KERNEL_DIR)/,*.cpp) $(wildcard $(SHELL_DIR)/*.cpp $(FILESYS_DIR)/*.cpp)
S_SRCS   := $(SHELL_DIR)/isr_wrappers.s
SP_SRCS  :=

C_OBJS   := $(patsubst %.c,   $(BUILD_DIR)/%.o, $(C_SRCS))
CPP_OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(CPP_SRCS))
S_OBJS   := $(patsubst %.s,   $(BUILD_DIR)/%.o, $(S_SRCS))
ASM_OBJS := $(S_OBJS)

KERNEL_ELF = $(BUILD_DIR)/kernel.elf
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
OS_IMG     = os.img

APP_SRCS  := $(wildcard $(APPS_DIR)/*.c)
APP_NAMES := $(patsubst $(APPS_DIR)/%.c, %, $(APP_SRCS))
APP_PEXES := $(patsubst %, $(BUILD_DIR)/apps/%.pexe, $(APP_NAMES))

WM_SRCS   := $(wildcard $(WM_DIR)/*.c)
WM_NAMES  := $(patsubst $(WM_DIR)/%.c, %, $(WM_SRCS))
WM_PEXES  := $(patsubst %, $(BUILD_DIR)/wm/%.pexe, $(WM_NAMES))

MKPEXE    = $(BUILD_DIR)/tools/mkpexe.exe
FSWRITE   = $(BUILD_DIR)/tools/fswrite.exe
FMTCHECK  = $(BUILD_DIR)/tools/fmtcheck.exe
CHECK_LINK_REGIONS = $(BUILD_DIR)/tools/check_link_regions.exe

HOSTTEST_SRCS := $(wildcard $(HOSTTEST_DIR)/*.c)
HOSTTEST_BINS := $(patsubst $(HOSTTEST_DIR)/%.c, $(BUILD_DIR)/$(HOSTTEST_DIR)/%.exe, $(HOSTTEST_SRCS))

# ======================================================================
# Rules
# ======================================================================
.PHONY: all clean fullclean run check inject apps wms boot_format flash hosttests run-hosttests

# Keep intermediate .elf files so make inject does not force a relink.
.SECONDARY: $(patsubst $(BUILD_DIR)/apps/%.pexe, $(BUILD_DIR)/apps/%.what shelf, $(APP_PEXES))
.SECONDARY: $(patsubst $(BUILD_DIR)/wm/%.pexe, $(BUILD_DIR)/wm/%.elf, $(WM_PEXES))

# flash = full rebuild + inject in one step. Use this the first time or
# whenever you change kernel code or filesystem layout.
# After "make flash", run "make run" to boot.
flash: fullclean all inject

all: $(OS_IMG) apps wms

# ---- Boot binary -----------------------------------------------------
$(BUILD_DIR)/boot.bin: $(BOOT_DIR)/boot.asm | $(BUILD_DIR)
	$(NASM) -f bin $< -o $@

# ---- Kernel objects --------------------------------------------------
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(call MAKE_DIR,$(dir $@))
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(call MAKE_DIR,$(dir $@))
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.s | $(BUILD_DIR)
	$(call MAKE_DIR,$(dir $@))
	$(CC) $(CFLAGS) -x assembler -c $< -o $@

$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)
	$(call MAKE_DIR,$(dir $@))
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

# ---- Link kernel -----------------------------------------------------
$(KERNEL_ELF): $(C_OBJS) $(CPP_OBJS) $(ASM_OBJS) | $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $^

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary --remove-section=.appbuf --remove-section=.appstack $< $@
	$(if $(POSIX_SHELL),@$(DU) -b $@ 2>/dev/null || ls -l $@,@$(DU) -b $@ 2>nul || dir $@)

# ---- Host tools ------------------------------------------------------
$(BUILD_DIR)/tools/mkpexe.exe: $(TOOLS_DIR)/mkpexe.c | $(BUILD_DIR)
	$(call MAKE_DIR,$(BUILD_DIR)/tools)
	$(HOSTCC) -O2 -o $@ $<

$(BUILD_DIR)/tools/fswrite.exe: $(TOOLS_DIR)/fswrite.c | $(BUILD_DIR)
	$(call MAKE_DIR,$(BUILD_DIR)/tools)
	$(HOSTCC) -O2 -o $@ $<

# fmtcheck: exits 0 if os.img is already formatted, 1 if not
$(BUILD_DIR)/tools/fmtcheck.exe: $(TOOLS_DIR)/fmtcheck.c | $(BUILD_DIR)
	$(call MAKE_DIR,$(BUILD_DIR)/tools)
	$(HOSTCC) -O2 -o $@ $<

# check_link_regions: reads `nm kernel.elf` on stdin, fails if a fixed
# linker.ld region (.appbuf/.appstack/.usertramp) has silently collapsed
# to zero width - see the tool's own header comment for the real bug
# this exists because of.
$(BUILD_DIR)/tools/check_link_regions.exe: $(TOOLS_DIR)/check_link_regions.c | $(BUILD_DIR)
	$(call MAKE_DIR,$(BUILD_DIR)/tools)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

# ---- Host tests --------------------------------------------------------
# Pure-logic checks that run directly on the host, not in QEMU - see
# each hosttest/*.c file's own header comment for exactly what it
# duplicates and why (freestanding-only kernel headers can't be pulled
# into a normal hosted build, so these intentionally re-derive just the
# logic under test rather than #including the real kernel .c files).
$(BUILD_DIR)/$(HOSTTEST_DIR)/%.exe: $(HOSTTEST_DIR)/%.c | $(BUILD_DIR)
	$(call MAKE_DIR,$(BUILD_DIR)/$(HOSTTEST_DIR))
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

# make hosttests     - build and run every hosttest/*.c, stopping at the
#                      first failure. Each test's own main() returns the
#                      number of FAILs (0 on success).
# make run-hosttests - compatibility alias for make hosttests.
hosttests: $(HOSTTEST_BINS)
	$(if $(POSIX_SHELL),@for test in $(HOSTTEST_BINS); do echo "=== $$test ==="; "./$$test" || exit 1; done,@for %%f in ($(subst /,\,$(HOSTTEST_BINS))) do (echo === %%f === & %%f || exit /b 1))
	@echo All hosttests passed.

run-hosttests: hosttests

# ---- App compilation -------------------------------------------------
$(BUILD_DIR)/apps/%.o: $(APPS_DIR)/%.c | $(BUILD_DIR)
	$(call MAKE_DIR,$(BUILD_DIR)/apps)
	$(CC) $(APPFLAGS) -c $< -o $@

$(BUILD_DIR)/apps/%.elf: $(BUILD_DIR)/apps/%.o
	$(LD) -m elf_i386 -Ttext $(APP_LOAD_ADDR) --entry=app_main -o $@ $^

# mkpexe reads e_entry from the ELF directly -- no .bin intermediate needed.
$(BUILD_DIR)/apps/%.pexe: $(BUILD_DIR)/apps/%.elf $(MKPEXE)
	$(MKPEXE) $< $@

apps: $(APP_PEXES)

# ---- WM compilation ----------------------------------------------------
# Same pexe format, same entry symbol and load address as a normal app -
# only the syscall table (wm_api_t vs app_api_t) and install directory
# (/wm vs /bin) differ. See kernel/wm_api.h.
$(BUILD_DIR)/wm/%.o: $(WM_DIR)/%.c | $(BUILD_DIR)
	$(call MAKE_DIR,$(BUILD_DIR)/wm)
	$(CC) $(APPFLAGS) -c $< -o $@

$(BUILD_DIR)/wm/%.elf: $(BUILD_DIR)/wm/%.o
	$(LD) -m elf_i386 -Ttext $(APP_LOAD_ADDR) --entry=app_main -o $@ $^

$(BUILD_DIR)/wm/%.pexe: $(BUILD_DIR)/wm/%.elf $(MKPEXE)
	$(MKPEXE) $< $@

wms: $(WM_PEXES)

# ---- OS image --------------------------------------------------------
$(OS_IMG): $(BUILD_DIR)/boot.bin $(KERNEL_BIN)
	$(if $(POSIX_SHELL),@test -f $(OS_IMG) || $(DD) if=/dev/zero of=$(OS_IMG) bs=512 count=8192 2>/dev/null,@if not exist $(OS_IMG) $(DD) if=/dev/zero of=$(OS_IMG) bs=512 count=8192 2>nul)
	$(if $(POSIX_SHELL),$(DD) if=$(BUILD_DIR)/boot.bin of=$(OS_IMG) bs=512 seek=0 conv=notrunc 2>/dev/null,$(DD) if=$(BUILD_DIR)/boot.bin of=$(OS_IMG) bs=512 seek=0 conv=notrunc 2>nul)
	$(if $(POSIX_SHELL),$(DD) if=$(KERNEL_BIN) of=$(OS_IMG) bs=512 seek=1 conv=notrunc 2>/dev/null,$(DD) if=$(KERNEL_BIN) of=$(OS_IMG) bs=512 seek=1 conv=notrunc 2>nul)

# ---- boot_format: boot QEMU headlessly to format the FS, then quit --
# Uses -no-reboot so QEMU exits on the first triple fault / shutdown.
# The OS runs, formats, then we inject apps.
# We use a 3-second timeout via a wrapper approach: just run normally
# for now and tell the user to quit after the prompt appears.
boot_format: $(OS_IMG) $(FMTCHECK)
	@echo Checking if filesystem is already formatted...
	@$(FMTCHECK) $(OS_IMG) && echo Already formatted, skipping boot_format. || ( \
		echo Not formatted. Booting to format... && \
		echo Close QEMU once you see the prompt, then make inject will run. \
	)

# ---- inject ----------------------------------------------------------
inject: apps wms $(FSWRITE)
	$(FSWRITE) $(OS_IMG) /bin/echo  		$(BUILD_DIR)/apps/echo.pexe
	$(FSWRITE) $(OS_IMG) /bin/ls    		$(BUILD_DIR)/apps/ls.pexe
	$(FSWRITE) $(OS_IMG) /bin/read  		$(BUILD_DIR)/apps/read.pexe
	$(FSWRITE) $(OS_IMG) /bin/write 		$(BUILD_DIR)/apps/write.pexe
	$(FSWRITE) $(OS_IMG) /bin/rm    		$(BUILD_DIR)/apps/rm.pexe
	$(FSWRITE) $(OS_IMG) /bin/mkdir 		$(BUILD_DIR)/apps/mkdir.pexe
	$(FSWRITE) $(OS_IMG) /bin/rmdir 		$(BUILD_DIR)/apps/rmdir.pexe
	$(FSWRITE) $(OS_IMG) /bin/tests 		$(BUILD_DIR)/apps/tests.pexe
	$(FSWRITE) $(OS_IMG) /bin/trig  		$(BUILD_DIR)/apps/trig.pexe
	$(FSWRITE) $(OS_IMG) /bin/ping          $(BUILD_DIR)/apps/ping.pexe
	$(FSWRITE) $(OS_IMG) /bin/fetch         $(BUILD_DIR)/apps/fetch.pexe
	$(FSWRITE) $(OS_IMG) /wm/tiled.pexe $(BUILD_DIR)/wm/tiled.pexe
	@echo Apps injected. Run 'make run' to boot.

# ---- Utilities -------------------------------------------------------
$(BUILD_DIR):
	$(call MAKE_DIR,$(BUILD_DIR))

clean:
	$(if $(POSIX_SHELL),-rm -rf "$(BUILD_DIR)",-if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)")

fullclean: clean
	$(if $(POSIX_SHELL),-rm -f "$(OS_IMG)",-if exist "$(OS_IMG)" del /q "$(OS_IMG)")

# nm's output is redirected to a file and re-read by check_link_regions
# rather than piped directly (`nm ... | check_link_regions`) - GNU
# Make's native Windows port doesn't reliably forward a `|` through
# cmd.exe from inside a recipe, so a pipe here works under Git Bash/
# POSIX_SHELL but can fail under a plain cmd.exe prompt with a
# confusing "'...' is not recognized" error that has nothing to do with
# either program actually being missing.
check: $(KERNEL_ELF) $(CHECK_LINK_REGIONS)
	$(if $(POSIX_SHELL),@$(DU) -b $(KERNEL_BIN),@$(DU) -b $(KERNEL_BIN))
	@echo Max safe size: 27648 bytes ^(54 sectors, stops before 0x7C00^)
	@$(call MAKE_DIR,$(BUILD_DIR)/tools)
	@$(NM) $(KERNEL_ELF) > $(BUILD_DIR)/tools/kernel_symbols.txt
	@$(CHECK_LINK_REGIONS) $(BUILD_DIR)/tools/kernel_symbols.txt

# "make run" intentionally does NOT depend on "all" so that re-running
# QEMU after "make inject" does not re-dd the kernel and overwrite the
# filesystem sectors written by fswrite.  Build first with "make", then
# "make inject", then "make run".
run:
	$(QEMU) -drive file=$(OS_IMG),format=raw -m 64 -nic user,model=rtl8139 -d in_asm -D boot.log