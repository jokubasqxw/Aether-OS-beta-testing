[bits 32]

MULTIBOOT_PAGE_ALIGN  equ 1 << 0
MULTIBOOT_MEMORY_INFO equ 1 << 1
MULTIBOOT_HEADER_MAGIC equ 0x1BADB002
MULTIBOOT_HEADER_FLAGS equ MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO
MULTIBOOT_CHECKSUM     equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

section .multiboot
align 4
    dd MULTIBOOT_HEADER_MAGIC
    dd MULTIBOOT_HEADER_FLAGS
    dd MULTIBOOT_CHECKSUM

section .text
global _start
extern kernel_main

_start:
    cli
    cld

    ; Multiboot passes:
    ; EAX = magic (0x2BADB002)
    ; EBX = pointer to multiboot_info
    mov [saved_magic], eax
    mov [saved_mbi], ebx

    ; Setup temporary stack
    mov esp, stack_top

    ; Check if CPUID is supported
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid

    ; Check if Long Mode is supported
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; Setup 4-level paging (PML4 -> PDPT -> 4 x PD with 2MB huge pages = 4GB identity)
    ; Clear page tables (PML4 + PDPT + 4 Page Directories = 24KB = 6144 dwords)
    mov edi, pml4_table
    mov ecx, 6144
    xor eax, eax
    rep stosd

    ; PML4[0] -> PDPT (Present | Writable)
    mov eax, pdpt_table
    or eax, 0x03
    mov [pml4_table], eax

    ; PDPT[0..3] -> 4 Page Directories
    mov eax, page_directories
    or eax, 0x03
    mov [pdpt_table + 0], eax
    add eax, 4096
    mov [pdpt_table + 8], eax
    add eax, 4096
    mov [pdpt_table + 16], eax
    add eax, 4096
    mov [pdpt_table + 24], eax

    ; Identity map first 4GB using 2048 x 2MB huge pages
    mov ecx, 0
.map_pd_loop:
    mov eax, 0x200000   ; 2MB
    mul ecx
    or eax, 0x83        ; Present | Writable | Huge (2MB)
    mov [page_directories + ecx * 8], eax
    mov dword [page_directories + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 2048
    jne .map_pd_loop

    ; Load CR3 with address of PML4
    mov eax, pml4_table
    mov cr3, eax

    ; Enable PAE (Physical Address Extension) in CR4
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Enable Long Mode in EFER MSR (0xC0000080)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable Paging in CR0
    mov eax, cr0
    or eax, (1 << 31) | (1 << 0) ; PG | PE
    mov cr0, eax

    ; Load temporary 64-bit GDT
    lgdt [gdt64_desc]

    ; Far jump to 64-bit Long Mode
    jmp 0x08:long_mode_start

.no_cpuid:
    mov byte [0xB8000], 'E'
    mov byte [0xB8001], 0x4F
    mov byte [0xB8002], '1'
    mov byte [0xB8003], 0x4F
    hlt
    jmp .no_cpuid

.no_long_mode:
    mov byte [0xB8000], 'E'
    mov byte [0xB8001], 0x4F
    mov byte [0xB8002], '2'
    mov byte [0xB8003], 0x4F
    hlt
    jmp .no_long_mode

[bits 64]
default rel
long_mode_start:
    ; Reload 64-bit data segment registers
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Setup 64-bit kernel stack
    mov rsp, stack_top

    ; Call kernel_main(magic, multiboot_info)
    ; System V AMD64 ABI: RDI = 1st arg, RSI = 2nd arg
    mov edi, [saved_magic]
    mov esi, [saved_mbi]
    call kernel_main

.halt_loop:
    cli
    hlt
    jmp .halt_loop

section .rodata
align 16
gdt64:
    dq 0 ; null descriptor
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) ; 0x08: Kernel 64-bit Code
    dq (1 << 41) | (1 << 44) | (1 << 47)             ; 0x10: Kernel 64-bit Data
gdt64_desc:
    dw $ - gdt64 - 1
    dq gdt64

section .data
saved_magic: dd 0
saved_mbi:   dd 0

section .bss
align 4096
global pml4_table
pml4_table:
    resb 4096
global pdpt_table
pdpt_table:
    resb 4096
global page_directories
page_directories:
    resb 4096 * 4 ; 16 KB for 4 Page Directories (maps 4 x 512 x 2MB = 4 GB)

align 16
stack_bottom:
    resb 65536 ; 64 KB kernel stack
stack_top:
