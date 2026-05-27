/*
 * userspace/user.ld — раскладка для userspace-программ.
 *
 * Адрес 0x40000000 = 1 GiB. Мы НЕ используем нижний 1 GiB,
 * потому что бутстрап его уже замапил identity через 2 MiB huge
 * pages БЕЗ флага USER. Нам нужны страницы с USER=1, проще
 * выделить новый диапазон.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : ALIGN(4K) {
        *(.text .text.*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss .bss.*)
    }

    _end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
}
