#include "block/ata.h"
#include "io.h"
#include "serial.h"

#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA0       0x1F3
#define ATA_LBA1       0x1F4
#define ATA_LBA2       0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_CTRL       0x3F6

#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DRQ     0x08
#define ATA_SR_ERR     0x01

#define ATA_CMD_READ   0x20
#define ATA_CMD_WRITE  0x30
#define ATA_CMD_FLUSH  0xE7

static int ata_wait_ready(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRDY))
            return 0;
        io_wait();
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & ATA_SR_ERR)
            return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ))
            return 0;
        io_wait();
    }
    return -1;
}

static void ata_select_lba(uint32_t lba, uint8_t count) {
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_init(void) {
    outb(ATA_CTRL, 0);
    if (ata_wait_ready() < 0) {
        serial_puts("[ata] primary disk not ready\n");
        return -1;
    }
    serial_puts("[ata] primary disk ready\n");
    return 0;
}

int ata_read_sector(uint32_t lba, void *buf) {
    return ata_read_sectors(lba, buf, 1);
}

int ata_read_sectors(uint32_t lba, void *buf, uint8_t count) {
    if (!buf || !count || lba > 0x0FFFFFFFu - (uint32_t)(count - 1u))
        return -1;
    if (ata_wait_ready() < 0)
        return -1;
    ata_select_lba(lba, count);
    outb(ATA_COMMAND, ATA_CMD_READ);
    uint16_t *out = (uint16_t *)buf;
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() < 0)
            return -1;
        io_insw(ATA_DATA, out + sector * 256u, 256);
    }
    return 0;
}

int ata_write_sector(uint32_t lba, const void *buf) {
    return ata_write_sectors(lba, buf, 1);
}

int ata_write_sectors(uint32_t lba, const void *buf, uint8_t count) {
    if (!buf || !count || lba > 0x0FFFFFFFu - (uint32_t)(count - 1u))
        return -1;
    if (ata_wait_ready() < 0)
        return -1;
    ata_select_lba(lba, count);
    outb(ATA_COMMAND, ATA_CMD_WRITE);
    const uint16_t *in = (const uint16_t *)buf;
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() < 0)
            return -1;
        io_outsw(ATA_DATA, in + sector * 256u, 256);
    }
    return ata_wait_ready();
}

int ata_flush_cache(void) {
    if (ata_wait_ready() < 0)
        return -1;
    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    return ata_wait_ready();
}
