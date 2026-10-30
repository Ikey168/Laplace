/* Laplace core: an AHCI (SATA) disk for the recording store (#236).
 *
 * Most current machines have no legacy IDE controller: their disks sit
 * behind an AHCI host bus adapter (PCI class 01:06:01). This driver finds the
 * first one on PCI, brings up the first port with a SATA disk attached, and
 * serves 512-byte sectors through the fat_block_device_t interface the
 * keyframe and journal store use, by polling: READ/WRITE DMA EXT through a
 * bounce buffer of whole frames, one PRDT entry per frame, and FLUSH CACHE EXT
 * after writes, so a write that returned survives a power cut.
 *
 * QEMU: -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0
 */

#ifndef CORE_AHCI_H
#define CORE_AHCI_H

#include "fat.h"

/* The first SATA disk behind an AHCI controller, or NULL. The boot log says
 * what was found. Call once. */
fat_block_device_t* ahci_boot_bind(void);

#endif /* CORE_AHCI_H */
