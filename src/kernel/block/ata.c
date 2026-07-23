#include "block/ata.h"
#include "io.h"
#include "serial.h"

#define ATA_REG_DATA       0
#define ATA_REG_ERROR      1
#define ATA_REG_SECCOUNT   2
#define ATA_REG_LBA0       3
#define ATA_REG_LBA1       4
#define ATA_REG_LBA2       5
#define ATA_REG_DRIVE      6
#define ATA_REG_STATUS     7
#define ATA_REG_COMMAND    7

#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DF      0x20
#define ATA_SR_DRQ     0x08
#define ATA_SR_ERR     0x01

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ   0x20
#define ATA_CMD_WRITE  0x30
#define ATA_CMD_FLUSH  0xE7

#define ATA_CTL_NIEN   0x02
#define ATA_POLL_LIMIT 100000

struct ata_device {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t slave;
    uint8_t present;
    uint32_t sectors;
};

static struct ata_device disk;

static uint16_t ata_port(uint8_t reg) {
    return (uint16_t)(disk.io_base + reg);
}

static void ata_delay_400ns(uint16_t ctrl_base) {
    (void)inb(ctrl_base);
    (void)inb(ctrl_base);
    (void)inb(ctrl_base);
    (void)inb(ctrl_base);
}

static int ata_wait_ready(void) {
    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t st = inb(ata_port(ATA_REG_STATUS));
        if (st == 0 || st == 0xFF)
            return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRDY))
            return 0;
        io_wait();
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t st = inb(ata_port(ATA_REG_STATUS));
        if (st == 0 || st == 0xFF || (st & (ATA_SR_DF | ATA_SR_ERR)))
            return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ))
            return 0;
        io_wait();
    }
    return -1;
}

static void ata_select(uint8_t head) {
    outb(ata_port(ATA_REG_DRIVE),
         (uint8_t)(0xE0 | (disk.slave << 4) | (head & 0x0F)));
    ata_delay_400ns(disk.ctrl_base);
}

static int ata_prepare_lba(uint32_t lba, uint8_t count) {
    ata_select((uint8_t)(lba >> 24));
    if (ata_wait_ready() < 0)
        return -1;
    outb(ata_port(ATA_REG_SECCOUNT), count);
    outb(ata_port(ATA_REG_LBA0), (uint8_t)(lba & 0xFF));
    outb(ata_port(ATA_REG_LBA1), (uint8_t)((lba >> 8) & 0xFF));
    outb(ata_port(ATA_REG_LBA2), (uint8_t)((lba >> 16) & 0xFF));
    return 0;
}

static int ata_identify(uint16_t io_base, uint16_t ctrl_base, uint8_t slave,
                        uint32_t *sectors_out) {
    uint16_t identify[256];

    outb(ctrl_base, ATA_CTL_NIEN);
    outb((uint16_t)(io_base + ATA_REG_DRIVE),
         (uint8_t)(0xA0 | (slave << 4)));
    ata_delay_400ns(ctrl_base);

    outb((uint16_t)(io_base + ATA_REG_SECCOUNT), 0);
    outb((uint16_t)(io_base + ATA_REG_LBA0), 0);
    outb((uint16_t)(io_base + ATA_REG_LBA1), 0);
    outb((uint16_t)(io_base + ATA_REG_LBA2), 0);
    outb((uint16_t)(io_base + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);

    uint8_t st = inb((uint16_t)(io_base + ATA_REG_STATUS));
    if (st == 0 || st == 0xFF)
        return -1;

    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        st = inb((uint16_t)(io_base + ATA_REG_STATUS));
        if (!(st & ATA_SR_BSY))
            break;
        if (i == ATA_POLL_LIMIT - 1)
            return -1;
        io_wait();
    }

    /* ATAPI devices answer IDENTIFY with an error and a non-zero signature.
     * BuzzOS needs a normal ATA disk, not an IDE CD-ROM. */
    if (inb((uint16_t)(io_base + ATA_REG_LBA1)) != 0 ||
        inb((uint16_t)(io_base + ATA_REG_LBA2)) != 0)
        return -1;

    for (int i = 0; i < ATA_POLL_LIMIT; i++) {
        st = inb((uint16_t)(io_base + ATA_REG_STATUS));
        if (st == 0 || st == 0xFF || (st & (ATA_SR_DF | ATA_SR_ERR)))
            return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ))
            break;
        if (i == ATA_POLL_LIMIT - 1)
            return -1;
        io_wait();
    }

    io_insw(io_base + ATA_REG_DATA, identify, 256);
    if (!(identify[49] & (1u << 9)))
        return -1;

    uint32_t sectors =
        (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    if (!sectors)
        return -1;
    *sectors_out = sectors;
    return 0;
}

int ata_init(void) {
    static const struct {
        uint16_t io_base;
        uint16_t ctrl_base;
    } channels[] = {
        {0x1F0, 0x3F6},
        {0x170, 0x376},
    };

    disk.present = 0;
    for (uint32_t channel = 0; channel < 2; channel++) {
        for (uint32_t slave = 0; slave < 2; slave++) {
            uint32_t sectors;
            if (ata_identify(channels[channel].io_base,
                             channels[channel].ctrl_base,
                             (uint8_t)slave, &sectors) < 0)
                continue;

            disk.io_base = channels[channel].io_base;
            disk.ctrl_base = channels[channel].ctrl_base;
            disk.slave = (uint8_t)slave;
            disk.sectors = sectors;
            disk.present = 1;

            serial_puts("[ata] disk found io=");
            serial_puthex(disk.io_base);
            serial_puts(disk.slave ? " slave sectors=" : " master sectors=");
            serial_puthex(disk.sectors);
            serial_puts("\n");
            return 0;
        }
    }

    serial_puts("[ata] no legacy IDE disk found\n");
    return -1;
}

int ata_read_sector(uint32_t lba, void *buf) {
    return ata_read_sectors(lba, buf, 1);
}

int ata_read_sectors(uint32_t lba, void *buf, uint8_t count) {
    if (!disk.present || !buf || !count ||
        lba > 0x0FFFFFFFu - (uint32_t)(count - 1u) ||
        lba + (uint32_t)count > disk.sectors)
        return -1;
    if (ata_prepare_lba(lba, count) < 0)
        return -1;
    outb(ata_port(ATA_REG_COMMAND), ATA_CMD_READ);
    uint16_t *out = (uint16_t *)buf;
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() < 0)
            return -1;
        io_insw(ata_port(ATA_REG_DATA), out + sector * 256u, 256);
    }
    return 0;
}

int ata_write_sector(uint32_t lba, const void *buf) {
    return ata_write_sectors(lba, buf, 1);
}

int ata_write_sectors(uint32_t lba, const void *buf, uint8_t count) {
    if (!disk.present || !buf || !count ||
        lba > 0x0FFFFFFFu - (uint32_t)(count - 1u) ||
        lba + (uint32_t)count > disk.sectors)
        return -1;
    if (ata_prepare_lba(lba, count) < 0)
        return -1;
    outb(ata_port(ATA_REG_COMMAND), ATA_CMD_WRITE);
    const uint16_t *in = (const uint16_t *)buf;
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() < 0)
            return -1;
        io_outsw(ata_port(ATA_REG_DATA), in + sector * 256u, 256);
    }
    return ata_wait_ready();
}

int ata_flush_cache(void) {
    if (!disk.present)
        return -1;
    ata_select(0);
    if (ata_wait_ready() < 0)
        return -1;
    outb(ata_port(ATA_REG_COMMAND), ATA_CMD_FLUSH);
    return ata_wait_ready();
}
