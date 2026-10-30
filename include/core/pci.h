/* Laplace core: PCI configuration space, mechanism #1 (ports 0xCF8/0xCFC). */

#ifndef CORE_PCI_H
#define CORE_PCI_H

#include <stdint.h>

static inline void pci_outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint32_t pci_inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint32_t pci_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
    pci_outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return pci_inl(0xCFC);
}

static inline void pci_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, uint32_t v) {
    pci_outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    pci_outl(0xCFC, v);
}

#endif /* CORE_PCI_H */
