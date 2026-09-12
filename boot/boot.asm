; Copyright (C) 2026 David S
; SPDX-License-Identifier: GPL-3.0-only

; boot.asm - V2.0 with VBE framebuffer support and colour masks
[org 0x7c00]
[bits 16]

KERNEL_OFFSET   equ 0x8000
SECTORS_TO_LOAD equ 200

; ------------------------------------------------------------------
; Fixed memory location where VBE info is stored for the kernel
; layout:
;   dword phys_base
;   dword width
;   dword height
;   dword bpp
;   dword pitch
;   dword red_mask_size
;   dword red_field_pos
;   dword green_mask_size
;   dword green_field_pos
;   dword blue_mask_size
;   dword blue_field_pos
; ------------------------------------------------------------------
VBE_INFO_ADDR  equ 0x5000

; Temporary buffer for VBE mode info - placed at 0x7E00 (just after boot sector)
VBE_MODE_BUF   equ 0x7E00

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov sp, 0x0600
    mov ss, ax
    mov [BOOT_DRIVE], dl

    ; Load kernel from disk
    mov dl, [BOOT_DRIVE]
    call disk_load

    ; ------------------------------------------------------------------
    ; VBE: set 1024x768x32 linear framebuffer
    ; ------------------------------------------------------------------
    mov ax, 0x4F01          ; VBE get mode info
    mov cx, 0x4118          ; mode number (bit 14 set = linear)
    mov di, VBE_MODE_BUF    ; buffer at 0x7E00
    int 0x10
    cmp ax, 0x004F
    jne vbe_error

    ; Set the mode
    mov ax, 0x4F02
    mov bx, 0x4118          ; same mode
    int 0x10
    cmp ax, 0x004F
    jne vbe_error

    ; Extract info from mode info block and store at VBE_INFO_ADDR (0x5000)
    mov si, VBE_MODE_BUF
    mov edi, VBE_INFO_ADDR

    ; physbase (dword) at +0x28
    mov eax, [si + 0x28]
    mov [edi], eax
    add edi, 4

    ; width (word) at +0x12
    movzx eax, word [si + 0x12]
    mov [edi], eax
    add edi, 4

    ; height (word) at +0x14
    movzx eax, word [si + 0x14]
    mov [edi], eax
    add edi, 4

    ; bpp (byte) at +0x19
    movzx eax, byte [si + 0x19]
    mov [edi], eax
    add edi, 4

    ; pitch (word) at +0x10
    movzx eax, word [si + 0x10]
    mov [edi], eax
    add edi, 4

    ; ---- Colour masks ----
    ; NOTE: per the VESA VBE 2.0+ ModeInfoBlock spec, the direct colour
    ; fields start at offset 0x1F, not 0x1A. Offsets 0x1A-0x1E are
    ; NumberOfBanks/MemoryModel/BankSize/NumberOfImagePages/Reserved1 -
    ; reading those as colour masks is what was scrambling every pixel.

    ; red mask size (byte) at +0x1F
    movzx eax, byte [si + 0x1F]
    mov [edi], eax
    add edi, 4

    ; red field position (byte) at +0x20
    movzx eax, byte [si + 0x20]
    mov [edi], eax
    add edi, 4

    ; green mask size (byte) at +0x21
    movzx eax, byte [si + 0x21]
    mov [edi], eax
    add edi, 4

    ; green field position (byte) at +0x22
    movzx eax, byte [si + 0x22]
    mov [edi], eax
    add edi, 4

    ; blue mask size (byte) at +0x23
    movzx eax, byte [si + 0x23]
    mov [edi], eax
    add edi, 4

    ; blue field position (byte) at +0x24
    movzx eax, byte [si + 0x24]
    mov [edi], eax

    jmp vbe_done
vbe_error:
    cli
    hlt
    jmp vbe_error
vbe_done:

    ; Enable A20 (fast A20)
    in  al, 0x92
    or  al, 2
    out 0x92, al

    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp CODE_SEG:init_pm

; ------------------------------------------------------------------
; disk_load: LBA read using INT 13h AH=42h
; ------------------------------------------------------------------
disk_load:
    pusha
    xor edi, edi              ; edi = sectors already loaded so far

.read_loop:
    mov eax, SECTORS_TO_LOAD
    sub eax, edi
    je  .done

    cmp eax, 64
    jbe .chunk_size_ok
    mov eax, 64
.chunk_size_ok:
    mov [dap_count], ax

    mov eax, edi
    inc eax
    mov [dap_lba], eax
    mov dword [dap_lba + 4], 0

    mov eax, edi
    shl eax, 9
    add eax, KERNEL_OFFSET
    shr eax, 4
    mov [dap_seg], ax
    mov word [dap_off], 0

    mov dl, [BOOT_DRIVE]
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc  disk_error

    movzx eax, word [dap_count]
    add edi, eax
    jmp .read_loop

.done:
    popa
    ret

disk_error:
    mov ah, 0x0e
    mov al, 'E'
    int 0x10
    jmp $

; Disk Address Packet
dap:
    db 0x10
    db 0
dap_count: dw 0
dap_off:   dw 0
dap_seg:   dw 0
dap_lba:   dq 0

; ------------------------------------------------------------------
; Protected Mode Entry
; ------------------------------------------------------------------
[bits 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ebp, 0x90000
    mov esp, ebp

    ; Enable FPU
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))
    or  eax, (1 << 1)
    mov cr0, eax
    finit

    ; Enable SSE
    mov eax, cr4
    or  eax, (1 << 9) | (1 << 10)
    mov cr4, eax

    ; Jump to kernel
    call KERNEL_OFFSET
    jmp $

; ------------------------------------------------------------------
; GDT
; ------------------------------------------------------------------
gdt_start:
    dd 0x0, 0x0
gdt_code:
    dw 0xffff, 0x0000
    db 0x00, 0x9a, 0xcf, 0x00
gdt_data:
    dw 0xffff, 0x0000
    db 0x00, 0x92, 0xcf, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

BOOT_DRIVE db 0

; ------------------------------------------------------------------
; Pad to 510 bytes and add boot signature
; ------------------------------------------------------------------
times 510-($-$$) db 0
dw 0xaa55