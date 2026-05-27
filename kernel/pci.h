#ifndef WEBOS_PCI_H
#define WEBOS_PCI_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u8  bus, dev, func;
    u16 vendor_id, device_id;
    u8  class_code, subclass;
    u32 bar[6];
} pci_device_t;

void pci_scan(void);

u32 pci_read32(u8 bus, u8 dev, u8 func, u8 off);
u16 pci_read16(u8 bus, u8 dev, u8 func, u8 off);
u8  pci_read8 (u8 bus, u8 dev, u8 func, u8 off);
void pci_write32(u8 bus, u8 dev, u8 func, u8 off, u32 val);

pci_device_t* pci_find_by_class(u8 class_code, u8 subclass);
pci_device_t* pci_find_by_id(u16 vendor, u16 device);
int  pci_count_devices(void);
pci_device_t* pci_get(int i);

#ifdef __cplusplus
}
#endif

#endif
