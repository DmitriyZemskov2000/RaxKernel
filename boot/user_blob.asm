; user_blob.asm — встраивает userspace ELF-image в kernel.
; ELF loader при старте разберёт program headers и замапит сам.

bits 64
section .rodata

global user_blob_start
global user_blob_end

user_blob_start:
    incbin "build/hello.elf"
user_blob_end:
