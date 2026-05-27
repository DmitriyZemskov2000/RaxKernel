/*
 * main.c — kernel_main, итерация 7.
 *
 * Добавлено к итер.5/6:
 *   - PCI enumeration
 *   - Настоящий ELF loader (program headers)
 *   - Подготовка argv/envp на user-стеке (SysV ABI)
 *   - Расширенная libc: getenv, sys/stat.h, sys/wait.h и т.д.
 *
 * Цель: подготовить фундамент для запуска GCC (и других больших программ).
 */

#include "types.h"
#include "vga.h"
#include "serial.h"
#include "idt.h"
#include "pmm.h"
#include "heap.h"
#include "vmm.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "sched.h"
#include "gdt.h"
#include "syscall.h"
#include "vfs.h"
#include "devfs.h"
#include "tarfs.h"
#include "ramfs.h"
#include "mb2.h"
#include "pci.h"
#include "virtio_blk.h"
#include "elf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void cxx_call_global_ctors(void);
extern char user_blob_start[];
extern char user_blob_end[];

#define USER_STACK_TOP  0x40800000ULL
#define USER_STACK_SIZE (32 * 1024)

/*
 * build_user_stack — конструирует SysV x86_64 initial process stack.
 *
 * Layout (от high к low):
 *   <строки argv/envp>
 *   <padding для выравнивания>
 *   auxv[]:  AT_NULL, 0
 *   envp:    NULL terminator
 *            envp[envc-1], ..., envp[0]
 *   argv:    NULL terminator
 *            argv[argc-1], ..., argv[0]
 *   argc                                    ← initial RSP
 *
 * На входе _start: (RSP) mod 16 == 0.
 */
static u64 build_user_stack(u64 stack_top,
                            const char* const* argv, int argc,
                            const char* const* envp, int envc) {
    char* sp = (char*)stack_top;

    char* argv_strs[16];
    char* envp_strs[16];
    if (argc > 16) argc = 16;
    if (envc > 16) envc = 16;

    /* Кладём строки */
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(argv[i]) + 1;
        sp -= l;
        memcpy(sp, argv[i], l);
        argv_strs[i] = sp;
    }
    for (int i = envc - 1; i >= 0; i--) {
        size_t l = strlen(envp[i]) + 1;
        sp -= l;
        memcpy(sp, envp[i], l);
        envp_strs[i] = sp;
    }

    /* Выравниваем sp на 16 */
    sp = (char*)((u64)sp & ~15ULL);

    /* Считаем сколько 8-байтовых слотов нам нужно:
       2 (auxv) + 1 (envp NULL) + envc + 1 (argv NULL) + argc + 1 (argc) */
    int slots = 2 + 1 + envc + 1 + argc + 1;

    /* Чтобы RSP_at_entry mod 16 == 0, нужно положить padding если
       slots нечётное (8-байт слоты, base aligned 16). */
    if (slots & 1) sp -= 8;

    /* auxv */
    sp -= 8; *(u64*)sp = 0;        /* a_un */
    sp -= 8; *(u64*)sp = 0;        /* AT_NULL */

    /* envp NULL */
    sp -= 8; *(u64*)sp = 0;
    /* envp pointers (last first) */
    for (int i = envc - 1; i >= 0; i--) {
        sp -= 8; *(u64*)sp = (u64)envp_strs[i];
    }

    /* argv NULL */
    sp -= 8; *(u64*)sp = 0;
    /* argv pointers */
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8; *(u64*)sp = (u64)argv_strs[i];
    }

    /* argc */
    sp -= 8; *(u64*)sp = (u64)argc;

    return (u64)sp;
}

static u64 load_userspace(u64* out_user_rsp) {
    size_t blob_size = (size_t)(user_blob_end - user_blob_start);
    printf("[user-loader] blob: %zu bytes\n", blob_size);

    u64 entry = 0;
    if (elf_load(user_blob_start, blob_size, &entry) != 0) {
        panic("ELF load failed");
    }
    printf("[user-loader] ELF entry = 0x%lx\n", entry);

    /* Стек */
    for (u64 a = USER_STACK_TOP - USER_STACK_SIZE; a < USER_STACK_TOP; a += 4096) {
        void* phys = pmm_alloc_page();
        if (!phys) panic("oom user stack");
        if (vmm_map(a, (u64)phys, VMM_WRITABLE | VMM_USER) != 0) {
            panic("vmm_map user stack");
        }
        memset((void*)a, 0, 4096);
    }

    /* Готовим argv/envp */
    const char* argv[] = { "hello", "arg1", "arg2" };
    const char* envp[] = {
        "PATH=/bin:/usr/bin",
        "HOME=/root",
        "TERM=vt100",
        "LANG=C",
    };
    u64 user_rsp = build_user_stack(USER_STACK_TOP,
                                    argv, 3,
                                    envp, 4);
    printf("[user-loader] stack 0x%lx..0x%lx, initial RSP = 0x%lx\n",
           USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, user_rsp);

    *out_user_rsp = user_rsp;
    return entry;
}

/* Рекурсивно копирует дерево из исходной FS (tarfs) в ramfs. */
static void import_dir_recursive(vnode_t* src_dir, vnode_t* dst_dir) {
    char name[64];
    for (size_t i = 0; ; i++) {
        int nlen = src_dir->ops->readdir(src_dir, i, name, sizeof(name));
        if (nlen < 0) break;
        vnode_t* src = src_dir->ops->lookup(src_dir, name);
        if (!src) continue;

        if (src->type == VNODE_DIR) {
            vnode_t* sub = ramfs_create_dir(dst_dir, name);
            if (sub) import_dir_recursive(src, sub);
        } else if (src->type == VNODE_FILE) {
            vnode_t* dst = ramfs_create_file(dst_dir, name);
            if (!dst) continue;
            char buf[512];
            off_t off = 0;
            while (1) {
                ssize_t r = src->ops->read(src, buf, sizeof(buf), off);
                if (r <= 0) break;
                dst->ops->write(dst, buf, (size_t)r, off);
                off += r;
            }
        }
    }
}

void kernel_main(void* multiboot_info) {
    serial_init();
    vga_init();

    printf("====================================\n");
    printf("            RaxOS  v0.15            \n");
    printf("   vmraxz — ext2 rw + tcc   \n");
    printf("====================================\n");

    idt_init();
    pmm_init(multiboot_info);

    u64 initrd_start = 0, initrd_end = 0;
    int have_initrd = mb2_find_module(multiboot_info, NULL,
                                      &initrd_start, &initrd_end);
    if (have_initrd) {
        printf("[boot] initrd module: 0x%lx..0x%lx (%lu KB)\n",
               initrd_start, initrd_end, (initrd_end - initrd_start) / 1024);
        pmm_reserve_range(initrd_start, initrd_end);
    }

    vmm_init();
    heap_init();

    /* Инициализируем графический фреймбуфер (если GRUB дал граф. режим) */
    {
        extern int fb_init(void*);
        extern int fb_is_ready(void);
        extern void fb_clear(u32);
        extern void fb_fill_rect(u32,u32,u32,u32,u32);
        extern void fb_draw_text_scaled(u32,u32,const char*,u32,int);
        extern void fb_draw_text(u32,u32,const char*,u32,u32,int);
        extern u32 fb_get_width(void);
        if (fb_init(multiboot_info) == 0 && fb_is_ready()) {
            /* Стартовый экран RaxOS */
            fb_clear(0x00101828);                          /* тёмно-синий фон */
            fb_fill_rect(0, 0, fb_get_width(), 60, 0x00203A5C); /* верхняя панель */
            fb_draw_text_scaled(40, 18, "RaxOS", 0x00FFFFFF, 3);
            fb_draw_text_scaled(40, 100, "Graphics framebuffer online", 0x0000FF88, 2);
            fb_draw_text(40, 160, "framebuffer driver: pixels, rects, text 8x8", 0x00C0C0C0, 0, 0);
            fb_draw_text(40, 180, "next: e1000 network, then UI", 0x00C0C0C0, 0, 0);
            /* Цветовая палитра-полоска */
            u32 cols[] = {0x00FF0000,0x0000FF00,0x000000FF,0x00FFFF00,0x0000FFFF,0x00FF00FF};
            for (int i = 0; i < 6; i++)
                fb_fill_rect(40 + i*60, 230, 50, 40, cols[i]);
        }
    }

    cxx_call_global_ctors();

    gdt_init();
    syscall_init();

    pic_remap(PIC_OFFSET_MASTER, PIC_OFFSET_SLAVE);
    pic_mask_all();
    pic_clear_mask(0);
    pit_init(100);
    printf("[boot] PIC remapped, PIT @ 100 Hz\n");

    /* НОВОЕ итерации 7: PCI scan */
    pci_scan();
    { extern int e1000_init(void); e1000_init();
      extern int e1000_is_ready(void);
      extern void e1000_get_mac(u8*);
      extern int e1000_send(const void*, u16);
      extern int e1000_receive(void*, u16);
      if (e1000_is_ready()) {
          /* Тест: шлём ARP who-has 10.0.2.2 (QEMU gateway), ждём ответ. */
          u8 my_mac[6]; e1000_get_mac(my_mac);
          u8 frame[42];
          memset(frame, 0, sizeof(frame));
          /* Ethernet: dst broadcast, src our mac, type ARP(0x0806) */
          for (int i=0;i<6;i++) frame[i]=0xFF;
          for (int i=0;i<6;i++) frame[6+i]=my_mac[i];
          frame[12]=0x08; frame[13]=0x06;
          /* ARP: htype=1 ptype=0x0800 hlen=6 plen=4 op=1(request) */
          frame[14]=0x00; frame[15]=0x01;
          frame[16]=0x08; frame[17]=0x00;
          frame[18]=6; frame[19]=4;
          frame[20]=0x00; frame[21]=0x01;
          for (int i=0;i<6;i++) frame[22+i]=my_mac[i];   /* sender mac */
          frame[28]=10; frame[29]=0; frame[30]=2; frame[31]=15;  /* sender IP 10.0.2.15 */
          /* target mac = 0, target IP = 10.0.2.2 (gateway) */
          frame[38]=10; frame[39]=0; frame[40]=2; frame[41]=2;
          /* шлём ARP несколько раз и поллим между ними */
          for (int rep=0; rep<3; rep++) {
              int sr = e1000_send(frame, sizeof(frame));
              printf("[net-test] ARP #%d sent: %s\n", rep, sr==0?"OK":"FAIL");
              for (volatile int w=0; w<3000000; w++) __asm__ volatile("pause");
          }
          /* Поллим ответ дольше + диагностика RX-кольца */
          u8 rxbuf[1600];
          int got = 0;
          extern u32 e1000_dbg_rdh(void);
          extern u32 e1000_dbg_rdt(void);
          for (volatile int spin=0; spin<20000000 && !got; spin++) {
              int n = e1000_receive(rxbuf, sizeof(rxbuf));
              if (n > 0) {
                  printf("[net-test] RX %d bytes, ethertype=%02x%02x\n",
                         n, rxbuf[12], rxbuf[13]);
                  if (rxbuf[12]==0x08 && rxbuf[13]==0x06)
                      printf("[net-test] ARP REPLY! gw MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                             rxbuf[22],rxbuf[23],rxbuf[24],rxbuf[25],rxbuf[26],rxbuf[27]);
                  got = 1;
              }
          }
          if (!got) printf("[net-test] нет ответа. RDH=%u RDT=%u\n",
                           e1000_dbg_rdh(), e1000_dbg_rdt());
      }
    }

    /* virtio-blk диск (итерация 10), ext2 монтируется ниже */
    virtio_blk_init();

    /* НОВОЕ итерации 8: ramfs корневая, initrd копируется в /
       плюс создаём /tmp /bin /etc */
    vnode_t* root = ramfs_init();
    vfs_set_root(root);

    /* Подключаем устройства в /dev (и дубль в корень для совместимости) */
    {
        extern vnode_t* devfs_get(const char*);
        vnode_t* dev_null = ramfs_create_file(root, "null");
        vnode_t* dev_zero = ramfs_create_file(root, "zero");
        vnode_t* dev_console = ramfs_create_file(root, "console");
        if (dev_null && devfs_get("null"))    *dev_null = *devfs_get("null");
        if (dev_zero && devfs_get("zero"))    *dev_zero = *devfs_get("zero");
        if (dev_console && devfs_get("console")) *dev_console = *devfs_get("console");
        vfs_init_stdio(devfs_console());

        /* Каталог /dev с устройствами (нужно для /dev/fb0 и т.п.) */
        vnode_t* devdir = ramfs_create_dir(root, "dev");
        if (devdir) {
            vnode_t* d_null = ramfs_create_file(devdir, "null");
            vnode_t* d_zero = ramfs_create_file(devdir, "zero");
            vnode_t* d_con  = ramfs_create_file(devdir, "console");
            vnode_t* d_fb0  = ramfs_create_file(devdir, "fb0");
            vnode_t* d_inp  = ramfs_create_file(devdir, "input");
            vnode_t* d_fbi  = ramfs_create_file(devdir, "fbinfo");
            if (d_null && devfs_get("null"))    *d_null = *devfs_get("null");
            if (d_zero && devfs_get("zero"))    *d_zero = *devfs_get("zero");
            if (d_con  && devfs_get("console")) *d_con  = *devfs_get("console");
            if (d_fb0  && devfs_get("fb0"))     *d_fb0  = *devfs_get("fb0");
            if (d_inp  && devfs_get("input"))   *d_inp  = *devfs_get("input");
            if (d_fbi  && devfs_get("fbinfo"))  *d_fbi  = *devfs_get("fbinfo");
        }
    }

    /* Стандартные директории */
    ramfs_create_dir(root, "tmp");
    ramfs_create_dir(root, "bin");
    ramfs_create_dir(root, "etc");

    /* Импортируем initrd в ramfs если есть */
    if (have_initrd) {
        vnode_t* tar_root = tarfs_init((void*)initrd_start,
                                       (size_t)(initrd_end - initrd_start));
        if (tar_root) {
            import_dir_recursive(tar_root, root);
            printf("[boot] initrd files copied to ramfs\n");
        }
    }
    printf("[boot] ramfs root mounted with /tmp /bin /etc\n");

    /* Монтируем ext2 с virtio-blk диска под /disk */
    if (virtio_blk_available()) {
        extern vnode_t* ext2_mount(void);
        vnode_t* ext2_root = ext2_mount();
        if (ext2_root) {
            extern vnode_t* ramfs_mount_at(vnode_t*, const char*, vnode_t*);
            ramfs_mount_at(root, "disk", ext2_root);
            printf("[boot] ext2 mounted at /disk\n");
        }
    }

    sched_init();

    u64 user_rsp = 0;
    u64 entry = load_userspace(&user_rsp);
    task_create_user("hello", entry, user_rsp);
    printf("[boot] user task created, enabling interrupts\n\n");

    __asm__ volatile("sti");

    /* Графический десктоп (Nuklear) временно отключён — тестируем
       запуск настоящего GTK3-приложения через user task. */
    {
        extern int fb_is_ready(void);
        extern void keyboard_init(void);
        extern u32 fb_get_width(void);
        extern u32 fb_get_height(void);
        extern void mouse_init(int, int);
        if (fb_is_ready()) {
            keyboard_init();
            mouse_init((int)fb_get_width(), (int)fb_get_height());
        }
    }

    for (;;) __asm__ volatile("hlt");
}
