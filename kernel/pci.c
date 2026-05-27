/*
 * pci.c — PCI configspace enumeration через legacy port I/O.
 *
 * PCI configspace access "mechanism #1":
 *   Адрес запроса: 0x80000000 | (bus<<16) | (device<<11) | (func<<8) | (offset & 0xFC)
 *   Пишем в порт 0xCF8, читаем из 0xCFC.
 *
 * Каждое устройство в 256-байтном configspace имеет:
 *   0x00 — Vendor ID (16 bits)
 *   0x02 — Device ID
 *   0x08 — Revision + Class code (3 bytes)
 *   0x0C — Header type (bit 7 = multifunc)
 *   0x10..0x24 — BAR0..BAR5
 *
 * Мы делаем простой bus 0 scan + multifunc-aware enumeration.
 * В будущем — рекурсивный обход за bridges.
 */

#include "types.h"
#include <stdio.h>
#include "io.h"
#include "pci.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

u32 pci_read32(u8 bus, u8 dev, u8 func, u8 off) {
    u32 addr = 0x80000000u |
               ((u32)bus  << 16) |
               ((u32)dev  << 11) |
               ((u32)func <<  8) |
               (off & 0xFCu);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

u16 pci_read16(u8 bus, u8 dev, u8 func, u8 off) {
    u32 v = pci_read32(bus, dev, func, off & 0xFC);
    return (u16)((v >> ((off & 2) * 8)) & 0xFFFF);
}

u8 pci_read8(u8 bus, u8 dev, u8 func, u8 off) {
    u32 v = pci_read32(bus, dev, func, off & 0xFC);
    return (u8)((v >> ((off & 3) * 8)) & 0xFF);
}

void pci_write32(u8 bus, u8 dev, u8 func, u8 off, u32 val) {
    u32 addr = 0x80000000u |
               ((u32)bus  << 16) |
               ((u32)dev  << 11) |
               ((u32)func <<  8) |
               (off & 0xFCu);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

static const char* pci_class_name(u8 class_code, u8 subclass) {
    switch (class_code) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (subclass) {
        case 0x00: return "SCSI";
        case 0x01: return "IDE";
        case 0x02: return "Floppy";
        case 0x05: return "ATA";
        case 0x06: return "SATA/AHCI";
        case 0x08: return "NVMe";
        default:   return "Mass Storage";
        }
    case 0x02: return "Network";
    case 0x03: return "Display";
    case 0x04: return "Multimedia";
    case 0x06:
        switch (subclass) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x04: return "PCI-to-PCI Bridge";
        default:   return "Bridge";
        }
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB";
        default:   return "Serial Bus";
        }
    default: return "Other";
    }
}

/* Таблица найденных устройств */
#define MAX_PCI_DEVICES 32
static pci_device_t pci_devices[MAX_PCI_DEVICES];
static int pci_count = 0;

static void pci_register(u8 bus, u8 dev, u8 func,
                         u16 vid, u16 did, u8 class_code, u8 subclass) {
    if (pci_count >= MAX_PCI_DEVICES) return;
    pci_device_t* d = &pci_devices[pci_count++];
    d->bus = bus;
    d->dev = dev;
    d->func = func;
    d->vendor_id = vid;
    d->device_id = did;
    d->class_code = class_code;
    d->subclass = subclass;

    /* Прочитаем BARs */
    for (int i = 0; i < 6; i++) {
        d->bar[i] = pci_read32(bus, dev, func, 0x10 + i * 4);
    }
}

void pci_scan(void) {
    printf("[pci] scanning bus 0...\n");
    for (u8 dev = 0; dev < 32; dev++) {
        u16 vid = pci_read16(0, dev, 0, 0x00);
        if (vid == 0xFFFF) continue;          /* нет устройства */

        u8 header_type = pci_read8(0, dev, 0, 0x0E);
        u8 funcs = (header_type & 0x80) ? 8 : 1;

        for (u8 func = 0; func < funcs; func++) {
            u16 v = pci_read16(0, dev, func, 0x00);
            if (v == 0xFFFF) continue;
            u16 d  = pci_read16(0, dev, func, 0x02);
            u8 cls = pci_read8 (0, dev, func, 0x0B);
            u8 sub = pci_read8 (0, dev, func, 0x0A);

            printf("[pci] %02x:%02x.%x  %04x:%04x  %s\n",
                   0, dev, func, v, d, pci_class_name(cls, sub));
            pci_register(0, dev, func, v, d, cls, sub);
        }
    }
    printf("[pci] %d devices found\n", pci_count);
}

pci_device_t* pci_find_by_class(u8 class_code, u8 subclass) {
    for (int i = 0; i < pci_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

pci_device_t* pci_find_by_id(u16 vendor, u16 device) {
    for (int i = 0; i < pci_count; i++) {
        if (pci_devices[i].vendor_id == vendor &&
            pci_devices[i].device_id == device) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

int pci_count_devices(void) { return pci_count; }
pci_device_t* pci_get(int i) {
    if (i < 0 || i >= pci_count) return NULL;
    return &pci_devices[i];
}
