/*
 * gdt.c — GDT, TSS и подготовка к syscall/sysret.
 *
 * Бутстрап (boot.asm) загрузил минимальный GDT с одним code-сегментом.
 * Теперь, в long mode и из C, можем построить нормальный GDT с
 * user-сегментами и TSS.
 *
 * Layout (важен для syscall/sysret!):
 *   слот 0: null
 *   слот 1: kernel code (ring 0)        — KCS = 0x08
 *   слот 2: kernel data (ring 0)        — KDS = 0x10
 *   слот 3: user data   (ring 3)        — UDS = 0x18 | 3 = 0x1B
 *   слот 4: user code   (ring 3)        — UCS = 0x20 | 3 = 0x23
 *   слот 5+6: TSS дескриптор (16 байт)
 *
 * Порядок user data → user code зафиксирован Intel:
 *   sysret устанавливает SS = STAR[63:48] + 8, CS = STAR[63:48] + 16.
 *   То есть STAR[63:48] = 0x10 даст SS=0x18, CS=0x20 — что и нужно.
 *
 * TSS в long mode используется в основном для одной вещи: TSS.RSP0
 * хранит RSP, на который CPU переключится при переходе ring3 → ring0
 * (через прерывание/syscall).
 */

#include "types.h"
#include <string.h>
#include <stdio.h>
#include "gdt.h"

#define GDT_ENTRIES 7   /* 0..6, TSS занимает 5 и 6 */

typedef struct PACKED {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
} gdt_entry_t;

typedef struct PACKED {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
    u32 base_upper;
    u32 reserved;
} tss_descriptor_t;

typedef struct PACKED {
    u16 limit;
    u64 base;
} gdtr_t;

typedef struct PACKED {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 io_map_base;
} tss_t;

static gdt_entry_t gdt[GDT_ENTRIES];
static gdtr_t      gdtr;
static tss_t       tss __attribute__((aligned(16)));

/* Стек, на который CPU переключится при ring3 → ring0 */
static u8 kernel_irq_stack[16 * 1024] __attribute__((aligned(16)));

static void gdt_set(int idx, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[idx].limit_low    = (u16)(limit & 0xFFFF);
    gdt[idx].base_low     = (u16)(base & 0xFFFF);
    gdt[idx].base_mid     = (u8)((base >> 16) & 0xFF);
    gdt[idx].access       = access;
    gdt[idx].granularity  = (u8)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[idx].base_high    = (u8)((base >> 24) & 0xFF);
}

static void tss_descriptor_set(int idx, u64 base, u32 limit) {
    tss_descriptor_t* d = (tss_descriptor_t*)&gdt[idx];
    d->limit_low   = (u16)(limit & 0xFFFF);
    d->base_low    = (u16)(base & 0xFFFF);
    d->base_mid    = (u8)((base >> 16) & 0xFF);
    d->access      = 0x89;          /* present, type=available 64-bit TSS */
    d->granularity = (u8)((limit >> 16) & 0x0F);
    d->base_high   = (u8)((base >> 24) & 0xFF);
    d->base_upper  = (u32)(base >> 32);
    d->reserved    = 0;
}

void gdt_init(void) {
    gdt_set(0, 0, 0, 0, 0);

    /* 1: kernel code ring 0, 64-bit */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xA0);
    /* 2: kernel data ring 0 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xA0);
    /* 3: user data ring 3 */
    gdt_set(3, 0, 0xFFFFF, 0xF2, 0xA0);
    /* 4: user code ring 3, 64-bit */
    gdt_set(4, 0, 0xFFFFF, 0xFA, 0xA0);

    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (u64)kernel_irq_stack + sizeof(kernel_irq_stack);
    tss.io_map_base = sizeof(tss);
    tss_descriptor_set(5, (u64)&tss, sizeof(tss) - 1);

    gdtr.limit = (u16)(sizeof(gdt) - 1);
    gdtr.base  = (u64)gdt;
    __asm__ volatile("lgdt %0" : : "m"(gdtr));

    /* Перезагрузим data-сегменты на новый KDS=0x10 */
    __asm__ volatile(
        "mov %0, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        :: "r"((u16)0x10) : "ax", "memory"
    );
    /* CS перезагружаем через far return: push CS, push RIP, lretq */
    __asm__ volatile(
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        ::: "rax", "memory"
    );

    __asm__ volatile("ltr %0" : : "r"((u16)(5 * 8)));

    printf("[gdt] loaded, TR=0x%x, default kernel stack @ %p\n",
           5 * 8, (void*)tss.rsp0);
}

void tss_set_kernel_stack(u64 rsp) { tss.rsp0 = rsp; }

u64 tss_get_default_kernel_stack(void) {
    return (u64)kernel_irq_stack + sizeof(kernel_irq_stack);
}
