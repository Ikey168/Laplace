/* IKOS IDE/ATA Storage Controller Driver Implementation
 * Issue #15 - Device Driver Framework
 * 
 * Complete implementation of IDE/ATA controller driver with
 * PIO data transfers and device identification.
 */

#include "ide_driver.h"
#include "device_manager.h"
#include "memory.h"
#include <string.h>

/* ================================
 * Global State
 * ================================ */

static bool g_ide_driver_initialized = false;
static ide_stats_t g_ide_stats = {0};
static ide_device_t* g_ide_devices[4] = {0}; /* Primary master/slave, Secondary master/slave */

/* ================================
 * Low-level I/O Functions
 * ================================ */

static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t data;
    __asm__ volatile("inb %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static inline void outw(uint16_t port, uint16_t data) {
    __asm__ volatile("outw %0, %1" : : "a"(data), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t data;
    __asm__ volatile("inw %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static void debug_print(const char* format, ...) {
    // Placeholder for debug printing
    (void)format;
}

/* Simple delay function */
static void ide_delay(void) {
    for (volatile int i = 0; i < 4; i++) {
        inb(0x80); /* Port 0x80 is typically used for delays */
    }
}

/* ================================
 * IDE Register Access
 * ================================ */

/**
 * Read IDE register
 */
uint8_t ide_read_reg(ide_device_t* ide_dev, uint8_t reg) {
    if (!ide_dev) return 0xFF;
    return inb(ide_dev->controller.io_base + reg);
}

/**
 * Write IDE register
 */
void ide_write_reg(ide_device_t* ide_dev, uint8_t reg, uint8_t value) {
    if (!ide_dev) return;
    outb(ide_dev->controller.io_base + reg, value);
}

/**
 * Read IDE control register
 */
uint8_t ide_read_ctrl(ide_device_t* ide_dev) {
    if (!ide_dev) return 0xFF;
    return inb(ide_dev->controller.ctrl_base + IDE_CTRL_ALT_STATUS);
}

/**
 * Write IDE control register
 */
void ide_write_ctrl(ide_device_t* ide_dev, uint8_t value) {
    if (!ide_dev) return;
    outb(ide_dev->controller.ctrl_base + IDE_CTRL_DEVICE_CTRL, value);
}

/**
 * Read data port (16-bit)
 */
uint16_t ide_read_data(ide_device_t* ide_dev) {
    if (!ide_dev) return 0xFFFF;
    return inw(ide_dev->controller.io_base + IDE_REG_DATA);
}

/**
 * Write data port (16-bit)
 */
void ide_write_data(ide_device_t* ide_dev, uint16_t value) {
    if (!ide_dev) return;
    outw(ide_dev->controller.io_base + IDE_REG_DATA, value);
}

/* ================================
 * Status and Error Handling
 * ================================ */

/**
 * Wait for controller to be ready
 */
int ide_wait_ready(ide_device_t* ide_dev, uint32_t timeout_ms) {
    if (!ide_dev) return IDE_ERROR_INVALID_PARAM;
    
    uint32_t timeout = timeout_ms * 1000; /* Convert to microseconds */
    
    while (timeout-- > 0) {
        uint8_t status = ide_read_reg(ide_dev, IDE_REG_STATUS);
        
        if (!(status & IDE_STATUS_BSY) && (status & IDE_STATUS_DRDY)) {
            return IDE_SUCCESS;
        }
        
        /* Simple delay */
        ide_delay();
    }
    
    return IDE_ERROR_TIMEOUT;
}

/**
 * Wait for data request
 */
int ide_wait_drq(ide_device_t* ide_dev, uint32_t timeout_ms) {
    if (!ide_dev) return IDE_ERROR_INVALID_PARAM;
    
    uint32_t timeout = timeout_ms * 1000;
    
    while (timeout-- > 0) {
        uint8_t status = ide_read_reg(ide_dev, IDE_REG_STATUS);
        
        if (status & IDE_STATUS_ERR) {
            return IDE_ERROR_DRIVE_ERROR;
        }
        
        if (status & IDE_STATUS_DRQ) {
            return IDE_SUCCESS;
        }
        
        ide_delay();
    }
    
    return IDE_ERROR_TIMEOUT;
}

/**
 * Check for error condition
 */
bool ide_check_error(ide_device_t* ide_dev) {
    if (!ide_dev) return true;
    
    uint8_t status = ide_read_reg(ide_dev, IDE_REG_STATUS);
    return (status & IDE_STATUS_ERR) ? true : false;
}

/**
 * Get error register value
 */
uint8_t ide_get_error(ide_device_t* ide_dev) {
    if (!ide_dev) return 0xFF;
    return ide_read_reg(ide_dev, IDE_REG_ERROR);
}

/* ================================
 * Drive Selection and Setup
 * ================================ */

/**
 * Select drive (0 = master, 1 = slave)
 */
int ide_select_drive(ide_device_t* ide_dev, uint8_t drive) {
    if (!ide_dev || drive > 1) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    uint8_t drive_select = (drive == 0) ? IDE_DRIVE_MASTER : IDE_DRIVE_SLAVE;
    ide_write_reg(ide_dev, IDE_REG_DRIVE_SELECT, drive_select);
    
    /* Wait for drive selection to take effect */
    ide_delay();
    
    return IDE_SUCCESS;
}

/**
 * Setup LBA addressing
 */
int ide_setup_lba(ide_device_t* ide_dev, uint8_t drive, uint64_t lba, uint16_t count) {
    if (!ide_dev || drive > 1) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    /* Check if drive supports LBA */
    if (!ide_dev->drives[drive].present) {
        return IDE_ERROR_NO_DRIVE;
    }
    
    /* For simplicity, use 28-bit LBA only for now */
    if (lba >= (1ULL << 28)) {
        return IDE_ERROR_BAD_SECTOR;
    }
    
    /* Select drive and enable LBA mode */
    uint8_t drive_head = (drive == 0) ? IDE_DRIVE_MASTER : IDE_DRIVE_SLAVE;
    drive_head |= IDE_DRIVE_LBA | ((lba >> 24) & 0x0F);
    
    ide_write_reg(ide_dev, IDE_REG_DRIVE_SELECT, drive_head);
    ide_write_reg(ide_dev, IDE_REG_SECTOR_COUNT, count & 0xFF);
    ide_write_reg(ide_dev, IDE_REG_LBA_LOW, lba & 0xFF);
    ide_write_reg(ide_dev, IDE_REG_LBA_MID, (lba >> 8) & 0xFF);
    ide_write_reg(ide_dev, IDE_REG_LBA_HIGH, (lba >> 16) & 0xFF);
    
    return IDE_SUCCESS;
}

/* ================================
 * Drive Identification
 * ================================ */

/**
 * Parse IDENTIFY response to extract drive information
 */
static void ide_parse_identify_data(uint16_t* data, ide_drive_info_t* info) {
    /* Clear info structure */
    memset(info, 0, sizeof(ide_drive_info_t));
    
    info->present = true;
    info->is_packet = (data[0] & 0x8000) ? true : false;
    
    /* Geometry information (words 1, 3, 6) */
    info->cylinders = data[1];
    info->heads = data[3];
    info->sectors_per_track = data[6];
    
    /* LBA support (word 49) */
    bool lba_supported = (data[49] & 0x0200) ? true : false;
    
    if (lba_supported) {
        /* Total LBA sectors (words 60-61) */
        info->total_sectors = ((uint64_t)data[61] << 16) | data[60];
        
        /* Check for 48-bit LBA (words 83, 100-103) */
        if (data[83] & 0x0400) {
            info->lba48_supported = true;
            info->total_sectors = ((uint64_t)data[103] << 48) |
                                 ((uint64_t)data[102] << 32) |
                                 ((uint64_t)data[101] << 16) |
                                 data[100];
        }
    } else {
        /* Calculate total sectors from CHS */
        info->total_sectors = (uint64_t)info->cylinders * info->heads * info->sectors_per_track;
    }
    
    info->bytes_per_sector = 512; /* Standard sector size */
    
    /* DMA support (word 49) */
    info->dma_supported = (data[49] & 0x0100) ? true : false;
    
    /* Extract strings (model, serial, firmware) */
    /* Model string (words 27-46) */
    for (int i = 0; i < 20; i++) {
        uint16_t word = data[27 + i];
        info->model[i * 2] = (word >> 8) & 0xFF;
        info->model[i * 2 + 1] = word & 0xFF;
    }
    info->model[40] = '\0';
    
    /* Serial number (words 10-19) */
    for (int i = 0; i < 10; i++) {
        uint16_t word = data[10 + i];
        info->serial[i * 2] = (word >> 8) & 0xFF;
        info->serial[i * 2 + 1] = word & 0xFF;
    }
    info->serial[20] = '\0';
    
    /* Firmware revision (words 23-26) */
    for (int i = 0; i < 4; i++) {
        uint16_t word = data[23 + i];
        info->firmware[i * 2] = (word >> 8) & 0xFF;
        info->firmware[i * 2 + 1] = word & 0xFF;
    }
    info->firmware[8] = '\0';
    
    /* Trim trailing spaces from strings */
    for (int i = 39; i >= 0 && (info->model[i] == ' ' || info->model[i] == '\0'); i--) {
        info->model[i] = '\0';
    }
    for (int i = 19; i >= 0 && (info->serial[i] == ' ' || info->serial[i] == '\0'); i--) {
        info->serial[i] = '\0';
    }
    for (int i = 7; i >= 0 && (info->firmware[i] == ' ' || info->firmware[i] == '\0'); i--) {
        info->firmware[i] = '\0';
    }
}

/**
 * Identify a specific drive
 */
int ide_identify_drive(ide_device_t* ide_dev, uint8_t drive) {
    if (!ide_dev || drive > 1) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    /* Select the drive */
    if (ide_select_drive(ide_dev, drive) != IDE_SUCCESS) {
        return IDE_ERROR_INVALID_PARAM;
    }

    /* No device answers: the status register floats (0xFF) or reads 0 when
     * the other drive on the channel is present. Don't wait out a timeout. */
    uint8_t probe = ide_read_reg(ide_dev, IDE_REG_STATUS);
    if (probe == 0x00 || probe == 0xFF) {
        return IDE_ERROR_NO_DRIVE;
    }

    /* Wait for drive to be ready */
    if (ide_wait_ready(ide_dev, 1000) != IDE_SUCCESS) {
        debug_print("IDE: Drive %d not ready\n", drive);
        return IDE_ERROR_NOT_READY;
    }
    
    /* Send IDENTIFY command */
    ide_write_reg(ide_dev, IDE_REG_COMMAND, IDE_CMD_IDENTIFY);
    
    /* Wait for command completion */
    if (ide_wait_drq(ide_dev, 1000) != IDE_SUCCESS) {
        /* Try IDENTIFY PACKET command for ATAPI devices */
        ide_write_reg(ide_dev, IDE_REG_COMMAND, IDE_CMD_IDENTIFY_PACKET);
        if (ide_wait_drq(ide_dev, 1000) != IDE_SUCCESS) {
            debug_print("IDE: Drive %d identification failed\n", drive);
            return IDE_ERROR_DRIVE_ERROR;
        }
    }
    
    /* Read identification data */
    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = ide_read_data(ide_dev);
    }
    
    /* Parse the identification data */
    ide_parse_identify_data(identify_data, &ide_dev->drives[drive]);
    
    debug_print("IDE: Drive %d identified: %s (%llu sectors)\n", 
                drive, ide_dev->drives[drive].model, ide_dev->drives[drive].total_sectors);
    
    g_ide_stats.drives_found++;
    return IDE_SUCCESS;
}

/**
 * Identify all drives on controller
 */
bool ide_drive_present(ide_device_t* ide_dev, uint8_t drive) {
    return ide_dev && drive < 2 && ide_dev->drives[drive].present;
}

int ide_identify_drives(ide_device_t* ide_dev) {
    if (!ide_dev) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    debug_print("IDE: Identifying drives on controller (base: 0x%x)\n", 
                ide_dev->controller.io_base);
    
    /* Reset drive info */
    memset(ide_dev->drives, 0, sizeof(ide_dev->drives));
    
    /* Try to identify master drive */
    ide_identify_drive(ide_dev, 0);
    
    /* Try to identify slave drive */
    ide_identify_drive(ide_dev, 1);
    
    return IDE_SUCCESS;
}

/* ================================
 * Data Transfer Operations
 * ================================ */

/**
 * Read sectors from drive
 */
int ide_read_sectors(ide_device_t* ide_dev, uint8_t drive, uint64_t lba, 
                     uint16_t count, void* buffer) {
    if (!ide_dev || drive > 1 || !buffer || count == 0) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    if (!ide_dev->drives[drive].present) {
        return IDE_ERROR_NO_DRIVE;
    }
    
    uint16_t* buf = (uint16_t*)buffer;
    
    /* Setup LBA addressing */
    if (ide_setup_lba(ide_dev, drive, lba, count) != IDE_SUCCESS) {
        return IDE_ERROR_BAD_SECTOR;
    }
    
    /* Wait for drive ready */
    if (ide_wait_ready(ide_dev, 1000) != IDE_SUCCESS) {
        return IDE_ERROR_NOT_READY;
    }
    
    /* Send read command */
    ide_write_reg(ide_dev, IDE_REG_COMMAND, IDE_CMD_READ_SECTORS);
    
    /* Read each sector */
    for (uint16_t sector = 0; sector < count; sector++) {
        /* Wait for data ready */
        if (ide_wait_drq(ide_dev, 5000) != IDE_SUCCESS) {
            g_ide_stats.read_errors++;
            return IDE_ERROR_TIMEOUT;
        }
        
        /* Check for errors */
        if (ide_check_error(ide_dev)) {
            uint8_t error = ide_get_error(ide_dev);
            debug_print("IDE: Read error: 0x%02x\n", error);
            g_ide_stats.read_errors++;
            return IDE_ERROR_DRIVE_ERROR;
        }
        
        /* Read sector data (256 words = 512 bytes) */
        for (int i = 0; i < 256; i++) {
            buf[sector * 256 + i] = ide_read_data(ide_dev);
        }
    }
    
    g_ide_stats.total_reads++;
    g_ide_stats.bytes_read += count * 512;
    
    return IDE_SUCCESS;
}

/**
 * Write sectors to drive
 */
int ide_write_sectors(ide_device_t* ide_dev, uint8_t drive, uint64_t lba, 
                      uint16_t count, const void* buffer) {
    if (!ide_dev || drive > 1 || !buffer || count == 0) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    if (!ide_dev->drives[drive].present) {
        return IDE_ERROR_NO_DRIVE;
    }
    
    const uint16_t* buf = (const uint16_t*)buffer;
    
    /* Setup LBA addressing */
    if (ide_setup_lba(ide_dev, drive, lba, count) != IDE_SUCCESS) {
        return IDE_ERROR_BAD_SECTOR;
    }
    
    /* Wait for drive ready */
    if (ide_wait_ready(ide_dev, 1000) != IDE_SUCCESS) {
        return IDE_ERROR_NOT_READY;
    }
    
    /* Send write command */
    ide_write_reg(ide_dev, IDE_REG_COMMAND, IDE_CMD_WRITE_SECTORS);
    
    /* Write each sector */
    for (uint16_t sector = 0; sector < count; sector++) {
        /* Wait for data ready */
        if (ide_wait_drq(ide_dev, 5000) != IDE_SUCCESS) {
            g_ide_stats.write_errors++;
            return IDE_ERROR_TIMEOUT;
        }
        
        /* Write sector data (256 words = 512 bytes) */
        for (int i = 0; i < 256; i++) {
            ide_write_data(ide_dev, buf[sector * 256 + i]);
        }
        
        /* Wait for write completion */
        if (ide_wait_ready(ide_dev, 5000) != IDE_SUCCESS) {
            g_ide_stats.write_errors++;
            return IDE_ERROR_TIMEOUT;
        }
        
        /* Check for errors */
        if (ide_check_error(ide_dev)) {
            uint8_t error = ide_get_error(ide_dev);
            debug_print("IDE: Write error: 0x%02x\n", error);
            g_ide_stats.write_errors++;
            return IDE_ERROR_DRIVE_ERROR;
        }
    }
    
    g_ide_stats.total_writes++;
    g_ide_stats.bytes_written += count * 512;
    
    return IDE_SUCCESS;
}

/**
 * Flush drive cache
 */
int ide_flush_cache(ide_device_t* ide_dev, uint8_t drive) {
    if (!ide_dev || drive > 1) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    if (!ide_dev->drives[drive].present) {
        return IDE_ERROR_NO_DRIVE;
    }
    
    /* Select drive */
    if (ide_select_drive(ide_dev, drive) != IDE_SUCCESS) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    /* Wait for ready */
    if (ide_wait_ready(ide_dev, 1000) != IDE_SUCCESS) {
        return IDE_ERROR_NOT_READY;
    }
    
    /* Send flush cache command */
    ide_write_reg(ide_dev, IDE_REG_COMMAND, IDE_CMD_FLUSH_CACHE);
    
    /* Wait for completion */
    if (ide_wait_ready(ide_dev, 30000) != IDE_SUCCESS) { /* Flush can take time */
        return IDE_ERROR_TIMEOUT;
    }
    
    return IDE_SUCCESS;
}

/* ================================
 * Controller Management
 * ================================ */

/**
 * Initialize IDE controller
 */
int ide_init_controller(ide_device_t* ide_dev, uint16_t io_base, uint16_t ctrl_base, uint8_t irq) {
    if (!ide_dev) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    /* Initialize controller structure */
    ide_dev->controller.io_base = io_base;
    ide_dev->controller.ctrl_base = ctrl_base;
    ide_dev->controller.irq = irq;
    ide_dev->controller.is_primary = (io_base == IDE_PRIMARY_BASE);
    
    /* Reset controller */
    ide_reset_controller(ide_dev);
    
    /* Identify drives */
    ide_identify_drives(ide_dev);
    
    ide_dev->initialized = true;
    g_ide_stats.controllers_found++;
    
    debug_print("IDE: Controller initialized (base: 0x%x, ctrl: 0x%x, irq: %d)\n",
                io_base, ctrl_base, irq);
    
    return IDE_SUCCESS;
}

/**
 * Reset IDE controller
 */
int ide_reset_controller(ide_device_t* ide_dev) {
    if (!ide_dev) {
        return IDE_ERROR_INVALID_PARAM;
    }
    
    debug_print("IDE: Resetting controller\n");
    
    /* Disable interrupts and assert reset */
    ide_write_ctrl(ide_dev, IDE_CTRL_nIEN | IDE_CTRL_SRST);
    
    /* Wait for reset */
    for (int i = 0; i < 10000; i++) {
        ide_delay();
    }
    
    /* Clear reset */
    ide_write_ctrl(ide_dev, IDE_CTRL_nIEN);
    
    /* Wait for drives to be ready */
    for (int i = 0; i < 100000; i++) {
        uint8_t status = ide_read_reg(ide_dev, IDE_REG_STATUS);
        if (!(status & IDE_STATUS_BSY)) {
            break;
        }
        ide_delay();
    }
    
    debug_print("IDE: Controller reset complete\n");
    return IDE_SUCCESS;
}

/* ================================
 * Driver Initialization
 * ================================ */

/**
 * Initialize IDE driver
 */
int ide_driver_init(void) {
    if (g_ide_driver_initialized) {
        return IDE_SUCCESS;
    }
    
    debug_print("IDE: Initializing IDE driver\n");
    
    /* Reset statistics */
    memset(&g_ide_stats, 0, sizeof(g_ide_stats));
    memset(g_ide_devices, 0, sizeof(g_ide_devices));
    
    g_ide_driver_initialized = true;
    
    debug_print("IDE: IDE driver initialized\n");
    return IDE_SUCCESS;
}

/**
 * Get IDE statistics
 */
void ide_get_stats(ide_stats_t* stats) {
    if (stats) {
        memcpy(stats, &g_ide_stats, sizeof(ide_stats_t));
    }
}

/**
 * Print drive information
 */
void ide_print_drive_info(const ide_drive_info_t* info) {
    if (!info || !info->present) {
        debug_print("Drive not present\n");
        return;
    }
    
    debug_print("Drive Information:\n");
    debug_print("  Model: %s\n", info->model);
    debug_print("  Serial: %s\n", info->serial);
    debug_print("  Firmware: %s\n", info->firmware);
    debug_print("  Capacity: %llu sectors (%llu MB)\n", 
                info->total_sectors, (info->total_sectors * 512) / (1024 * 1024));
    debug_print("  Geometry: %d cylinders, %d heads, %d sectors/track\n",
                info->cylinders, info->heads, info->sectors_per_track);
    debug_print("  Features: LBA48=%s, DMA=%s, ATAPI=%s\n",
                info->lba48_supported ? "Yes" : "No",
                info->dma_supported ? "Yes" : "No",
                info->is_packet ? "Yes" : "No");
}
