/*
 * SWEET eMMC root device (read-only).
 *
 * The Qualcomm SDHCI host (MSM8992) is left enabled by the bootloader,
 * which loaded our boot image from the eMMC. This driver reuses that state:
 * it issues polled MMC commands to the card to read sectors.
 *
 * Based on NativeSdhci from UniXNU-lumia950xl (MSM8992, same as LG G4 H815).
 *
 * Root on eMMC with rd=emmc0.
 *
 * Reads only - writes are refused.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>

/* SDHCI register offsets (from SdhciRegs.h) */
#define HC_BASE         0xf9824900ULL
#define HC_SIZE         0x1a0
#define CORE_BASE       0xf9824000ULL
#define CORE_SIZE       0x800

/* Standard SDHCI registers */
#define HC_CTRL1        0x28
#define HC_CLK_CTRL     0x2c
#define HC_TIMEOUT_CTRL 0x2e
#define HC_POWER_CTRL   0x29
#define HC_CAPABILITIES 0x40
#define HC_CAPABILITIES_2 0x44
#define HC_HOST_CTRL2   0x3c
#define HC_PRESENT      0x24
#define HC_INT_STATUS   0x30
#define HC_INT_ENABLE   0x34
#define HC_SIGNAL_ENABLE 0x38
#define HC_BLOCK_SIZE   0x04
#define HC_BLOCK_COUNT  0x06
#define HC_ARGUMENT     0x08
#define HC_COMMAND      0x0c
#define HC_RESPONSE     0x10
#define HC_BUFFER       0x20

/* SDHCI flags */
#define PS_CARD_PRESENT     (1u << 16)
#define PS_CMD_INHIBIT      (1u << 0)
#define PS_DAT_INHIBIT      (1u << 1)
#define PS_DAT0_LEVEL       (1u << 20)

#define INT_CMD_COMPLETE    (1u << 0)
#define INT_DATA_COMPLETE   (1u << 1)
#define INT_ERROR           (1u << 15)

#define CMD_RESP_NONE       0x00
#define CMD_RESP_R1         0x01
#define CMD_RESP_R1B        0x02
#define CMD_RESP_R2         0x03
#define CMD_RESP_R3         0x04

/* MMC commands */
#define CMD_GO_IDLE         0
#define CMD_SEND_OP_COND    1
#define CMD_ALL_SEND_CID    2
#define CMD_SET_RELATIVE_ADDR 3
#define CMD_SELECT_CARD     7
#define CMD_SEND_STATUS     13
#define CMD_READ_SINGLE     17
#define CMD_SET_BLOCKLEN    16

static volatile uint32_t *hc_base;
static uint16_t card_rca;
static int emmc_initialized = 0;

/* Buffer for reads */
#define EMMC_BUFFER_SIZE    (1u << 20)  /* 1 MB bounce buffer */
static void *emmc_buffer;

static inline uint32_t rd32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void wr32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint8_t rd8(uintptr_t addr)
{
    return *(volatile uint8_t *)addr;
}

static inline void wr8(uintptr_t addr, uint8_t val)
{
    *(volatile uint8_t *)addr = val;
}

static void emmc_delay(uint32_t us)
{
    delay(us);
}

static int emmc_cmd(uint8_t cmd, uint32_t arg, uint8_t resp_type, uint32_t *resp)
{
    uint32_t reg;
    int timeout = 100000;
    
    /* Wait for cmd inhibit */
    while ((rd32(HC_BASE + HC_PRESENT) & PS_CMD_INHIBIT) && timeout--) {
        emmc_delay(1);
    }
    if (timeout <= 0) {
        printf("emmc: CMD%d cmd inhibit timeout\n", cmd);
        return -1;
    }
    
    /* Clear interrupts */
    wr32(HC_BASE + HC_INT_STATUS, 0xFFFFFFFF);
    
    /* Set argument */
    wr32(HC_BASE + HC_ARGUMENT, arg);
    
    /* Set command: resp type + enable */
    reg = (cmd & 0x3F) | (1u << 8);  /* Command index check enable */
    switch (resp_type) {
        case CMD_RESP_NONE:
            break;
        case CMD_RESP_R1:
        case CMD_RESP_R1B:
            reg |= (1u << 6);  /* CRC check */
            reg |= (1u << 7);  /* Index check */
            break;
        case CMD_RESP_R2:
            reg |= (1u << 6);  /* CRC check */
            break;
    }
    wr32(HC_BASE + HC_COMMAND, reg);
    
    /* Wait for complete */
    timeout = 100000;
    while (!(rd32(HC_BASE + HC_INT_STATUS) & INT_CMD_COMPLETE) && timeout--) {
        emmc_delay(1);
    }
    
    if (timeout <= 0) {
        printf("emmc: CMD%d timeout\n", cmd);
        return -1;
    }
    
    /* Check for error */
    if (rd32(HC_BASE + HC_INT_STATUS) & INT_ERROR) {
        printf("emmc: CMD%d error, status=0x%x\n", cmd, rd32(HC_BASE + HC_INT_STATUS));
        return -1;
    }
    
    /* Clear interrupt */
    wr32(HC_BASE + HC_INT_STATUS, INT_CMD_COMPLETE);
    
    /* Read response */
    if (resp && resp_type != CMD_RESP_NONE) {
        resp[0] = rd32(HC_BASE + HC_RESPONSE);
        if (resp_type == CMD_RESP_R2) {
            resp[1] = rd32(HC_BASE + HC_RESPONSE + 4);
            resp[2] = rd32(HC_BASE + HC_RESPONSE + 8);
            resp[3] = rd32(HC_BASE + HC_RESPONSE + 12);
        }
    }
    
    return 0;
}

static int emmc_init(void)
{
    uint32_t resp[4];
    uint32_t ocr;
    int i;
    
    printf("emmc: initializing...\n");
    
    /* Map registers */
    hc_base = (volatile uint32_t *)ml_io_map(HC_BASE, HC_SIZE);
    if (!hc_base) {
        printf("emmc: failed to map HC registers\n");
        return -1;
    }
    
    /* Allocate bounce buffer */
    emmc_buffer = (void *)kmem_alloc(kernel_map, EMMC_BUFFER_SIZE);
    if (!emmc_buffer) {
        printf("emmc: failed to allocate buffer\n");
        return -1;
    }
    
    /* Card identification sequence */
    /* CMD0: GO_IDLE_STATE */
    if (emmc_cmd(CMD_GO_IDLE, 0, CMD_RESP_NONE, NULL) != 0) {
        printf("emmc: CMD0 failed\n");
        return -1;
    }
    emmc_delay(1000);
    
    /* CMD1: SEND_OP_COND (polling for busy) */
    for (i = 0; i < 1000; i++) {
        ocr = 0x40FF8000;  /* 3.3V, high capacity */
        if (emmc_cmd(CMD_SEND_OP_COND, ocr, CMD_RESP_R3, &resp[0]) != 0)
            break;
        if (resp[0] & 0x80000000) {  /* Power up complete */
            break;
        }
        emmc_delay(1000);
    }
    
    if (i >= 1000) {
        printf("emmc: CMD1 failed (could not get OCR)\n");
        return -1;
    }
    
    /* CMD2: SEND_CID */
    if (emmc_cmd(CMD_ALL_SEND_CID, 0, CMD_RESP_R2, resp) != 0) {
        printf("emmc: CMD2 failed\n");
        return -1;
    }
    printf("emmc: CID = %08x %08x %08x %08x\n", resp[0], resp[1], resp[2], resp[3]);
    
    /* CMD3: SET_RELATIVE_ADDR */
    card_rca = 2;  /* Use RCA 2, same as LK */
    if (emmc_cmd(CMD_SET_RELATIVE_ADDR, (uint32_t)card_rca << 16, CMD_RESP_R1, resp) != 0) {
        printf("emmc: CMD3 failed\n");
        return -1;
    }
    
    /* CMD7: SELECT_CARD */
    if (emmc_cmd(CMD_SELECT_CARD, (uint32_t)card_rca << 16, CMD_RESP_R1B, resp) != 0) {
        printf("emmc: CMD7 failed\n");
        return -1;
    }
    
    /* CMD16: SET_BLOCKLEN (512 bytes) */
    if (emmc_cmd(CMD_SET_BLOCKLEN, 512, CMD_RESP_R1, resp) != 0) {
        printf("emmc: CMD16 failed\n");
        return -1;
    }
    
    emmc_initialized = 1;
    printf("emmc: initialized, RCA=%d\n", card_rca);
    return 0;
}

static int emmc_read_block(uint32_t lba, void *buf)
{
    uint32_t resp[4];
    uint32_t reg;
    int timeout;
    
    /* Set block count */
    wr16(HC_BASE + HC_BLOCK_COUNT, 1);
    
    /* Set block size */
    wr16(HC_BASE + HC_BLOCK_SIZE, 512);
    
    /* Set argument (single block read) */
    wr32(HC_BASE + HC_ARGUMENT, lba);
    
    /* Wait for data inhibit */
    timeout = 100000;
    while ((rd32(HC_BASE + HC_PRESENT) & PS_DAT_INHIBIT) && timeout--) {
        emmc_delay(1);
    }
    if (timeout <= 0) {
        printf("emmc: read data inhibit timeout\n");
        return -1;
    }
    
    /* Clear interrupts */
    wr32(HC_BASE + HC_INT_STATUS, 0xFFFFFFFF);
    
    /* Send CMD17: READ_SINGLE_BLOCK */
    reg = CMD_READ_SINGLE | (1u << 8);  /* Enable */
    wr32(HC_BASE + HC_COMMAND, reg);
    
    /* Wait for data complete */
    timeout = 100000;
    while (!(rd32(HC_BASE + HC_INT_STATUS) & (INT_DATA_COMPLETE | INT_ERROR)) && timeout--) {
        emmc_delay(1);
    }
    
    if (timeout <= 0 || (rd32(HC_BASE + HC_INT_STATUS) & INT_ERROR)) {
        printf("emmc: read timeout/error, status=0x%x\n", rd32(HC_BASE + HC_INT_STATUS));
        return -1;
    }
    
    /* Read data from buffer port - 512 bytes */
    volatile uint32_t *buf32 = (volatile uint32_t *)HC_BASE + (HC_BUFFER / 4);
    uint32_t *dst = (uint32_t *)buf;
    for (int i = 0; i < 128; i++) {  /* 512 bytes / 4 */
        dst[i] = buf32[i];
    }
    
    /* Clear interrupt */
    wr32(HC_BASE + HC_INT_STATUS, INT_DATA_COMPLETE);
    
    return 0;
}

/* BSD device operations */
static int emmc_open(dev_t dev, int flags, int fmt, proc_t p)
{
    if (!emmc_initialized) {
        if (emmc_init() != 0) {
            return ENXIO;
        }
    }
    return 0;
}

static int emmc_close(dev_t dev, int flags, int fmt, proc_t p)
{
    return 0;
}

static int emmc_read(dev_t dev, struct uio *uio, int ioflag)
{
    uint64_t offset = uio->uio_offset;
    int n = uio->uio_resid;
    int error = 0;
    
    if (!emmc_initialized)
        return ENXIO;
    
    /* Read in at most EMMC_BUFFER_SIZE chunks */
    while (n > 0 && !error) {
        int chunk = (n > EMMC_BUFFER_SIZE) ? EMMC_BUFFER_SIZE : n;
        uint32_t lba = offset / 512;
        int blocks = chunk / 512;
        
        if (blocks == 1) {
            error = emmc_read_block(lba, emmc_buffer);
        } else {
            /* Multiple blocks - read one at a time for simplicity */
            for (int i = 0; i < blocks && !error; i++) {
                error = emmc_read_block(lba + i, emmc_buffer + i * 512);
            }
        }
        
        if (!error) {
            error = uiomove(emmc_buffer, chunk, uio);
        }
        
        offset += chunk;
        n -= chunk;
    }
    
    return error;
}

static int emmc_write(dev_t dev, struct uio *uio, int ioflag)
{
    /* Read-only */
    return EROFS;
}

static struct cdevsw emmc_cdevsw = {
    .d_open = emmc_open,
    .d_close = emmc_close,
    .d_read = emmc_read,
    .d_write = emmc_write,
    .d_ioctl = noioctl,
    .d_stop = nostop,
    .d_reset = noreset,
    .d_ttys = NULL,
    .d_select = noselect,
    .d_mmap = nommap,
    .d_strategy = nostrategy,
    .d_awake = NULL,
    .d_bcount = 0,
    .d_type = 0
};

static int emmc_console = 0;

static void emmc_cdevsw_init(void)
{
    dev_t dev = makedev(12, 0);  /* Major number 12 for disk */
    devfs_make_node(dev, DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0660, "emmc0");
    printf("emmc: devfs node created at /dev/emmc0\n");
}

/* Called from IOKitBSDInit for rd=emmc0 */
int sweet_emmc_rootdev(dev_t *dev)
{
    if (!emmc_initialized) {
        if (emmc_init() != 0) {
            printf("emmc: initialization failed\n");
            return ENXIO;
        }
    }
    
    if (!emmc_console) {
        emmc_cdevsw_init();
        emmc_console = 1;
    }
    
    *dev = makedev(12, 0);
    return 0;
}

#endif /* ARM64_BOARD_CONFIG_SWEET */
