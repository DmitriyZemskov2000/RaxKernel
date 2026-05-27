bits 64
section .init
global _init
_init:
    push rbp
    mov rbp, rsp
    ; (тело .init вставляется линкером между crti и crtn)

section .fini
global _fini
_fini:
    push rbp
    mov rbp, rsp
