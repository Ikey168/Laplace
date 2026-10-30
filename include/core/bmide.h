/* Laplace core: bus-master DMA for the IDE disk (#236).
 *
 * The IDE driver (kernel/ide_driver.c) moves every sector through the data
 * port. Under KVM each OUTS element is an exit to QEMU, 256 of them a
 * sector, and on a nested-virtualization host (a cloud CI runner) a keyframe
 * then takes minutes to write. A PCI IDE controller with bus mastering
 * (PIIX, class 01:01 with prog-if bit 7) moves a whole command's data
 * itself: READ/WRITE DMA EXT through a bounce buffer of whole frames, one
 * PRD entry per frame, polled like the AHCI driver.
 *
 * Only the primary channel in compatibility mode (ports 0x1F0/0x3F6), the
 * one the IDE driver brought up, is used.
 */

#ifndef CORE_BMIDE_H
#define CORE_BMIDE_H

#include "fat.h"

/* `pio` is the IDE disk bound by checkpoint_ide_boot_bind. A device that
 * moves the same sectors by DMA, once a test read matches the PIO one; NULL
 * when the controller has no bus master or the test fails (keep `pio`). */
fat_block_device_t* bmide_boot_bind(fat_block_device_t* pio);

#endif /* CORE_BMIDE_H */
