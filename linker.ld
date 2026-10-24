ENTRY(_start)

SECTIONS {
    . = 1M;
    __kernel_start = .;

    .multiboot : ALIGN(4K) {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4K) {
        *(.text*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata*)
        PROVIDE(kernel_version = "AetherOS 64-bit (Beta) v1.0.0");
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4K);
    __kernel_end = .;
}
