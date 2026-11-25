/*
 * displio-rebirth hardware probe (runs once, then removes itself).
 *
 * Linked at 0x08004000 (application area of the Displio bootloader).
 * Compiled for Cortex-M0 so the same binary runs on any Cortex-M core.
 *
 * What it does:
 *   1. masks interrupts, identifies the core (CPUID) and the STM32 family
 *      (DBGMCU IDCODE, flash controller layout);
 *   2. writes a report to flash at REPORT_ADDR: ids, flash size, UID,
 *      RCC/GPIO register snapshots, option bytes and a copy of the
 *      16 KiB bootloader;
 *   3. from RAM, erases the flash page/sector holding its own vector table
 *      (0x08004000) so the bootloader finds an empty application again,
 *      then resets.
 *
 * Any fault jumps to the same self-erase routine. The report survives because
 * it lives in another page/sector (0x08010000 = F2/F4 sector 4).
 */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))

#define APP_BASE    0x08004000u
#define REPORT_ADDR 0x08010000u
#define BOOT_BASE   0x08000000u
#define BOOT_SIZE   0x4000u

#define MAGIC 0x42525044u /* "DPRB" */
#define DONE  0x454E4F44u /* "DONE" */

enum { FAM_UNKNOWN = 0, FAM_F1 = 1, FAM_F0F3 = 2, FAM_F2F4 = 3 };

/* Report layout (offsets from REPORT_ADDR) */
#define R_HDR      0x000 /* magic, version, family, cpuid, idcode, flashsize, uid0..2 */
#define R_STATE    0x040 /* sp, vtor, primask, control, nvic iser0/1, systick ctrl */
#define R_OPT      0x080 /* option bytes, 32 bytes */
#define R_RCC      0x100 /* 0x100 bytes */
#define R_GPIO     0x200 /* 8 ports x 0x30 bytes */
#define R_AFIO     0x380 /* F1 AFIO, 0x20 bytes */
#define R_BOOT     0x400 /* 16 KiB bootloader copy */
#define R_DONE     (R_BOOT + BOOT_SIZE)

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

static volatile uint32_t g_family;
static volatile uint32_t g_sp_entry;

/* ------------------------------------------------------------------------ */
/* Flash helpers (run from flash; programming another page only stalls CPU) */

#define F1_FLASH 0x40022000u
#define F4_FLASH 0x40023C00u
#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu

static inline void iwdg_kick(void) { REG(0x40003000u) = 0xAAAAu; }

static void flash_unlock(uint32_t fam)
{
    if (fam == FAM_F2F4) {
        if (REG(F4_FLASH + 0x10) & (1u << 31)) {
            REG(F4_FLASH + 0x04) = FLASH_KEY1;
            REG(F4_FLASH + 0x04) = FLASH_KEY2;
        }
        REG(F4_FLASH + 0x0C) = 0xF3u; /* clear EOP/OPERR/WRPERR/PGAERR/PGPERR/PGSERR */
    } else {
        if (REG(F1_FLASH + 0x10) & (1u << 7)) {
            REG(F1_FLASH + 0x04) = FLASH_KEY1;
            REG(F1_FLASH + 0x04) = FLASH_KEY2;
        }
        REG(F1_FLASH + 0x0C) = 0x34u; /* clear EOP/WRPRTERR/PGERR */
    }
}

static void flash_write_word(uint32_t fam, uint32_t addr, uint32_t val)
{
    if (*(volatile uint32_t *)addr == val)
        return;
    iwdg_kick();
    if (fam == FAM_F2F4) {
        REG(F4_FLASH + 0x10) = (2u << 8) | 1u; /* PSIZE x32, PG */
        *(volatile uint32_t *)addr = val;
        while (REG(F4_FLASH + 0x0C) & (1u << 16))
            iwdg_kick();
        REG(F4_FLASH + 0x10) = 0;
    } else {
        REG(F1_FLASH + 0x10) = 1u; /* PG */
        *(volatile uint16_t *)addr = (uint16_t)val;
        while (REG(F1_FLASH + 0x0C) & 1u)
            iwdg_kick();
        *(volatile uint16_t *)(addr + 2) = (uint16_t)(val >> 16);
        while (REG(F1_FLASH + 0x0C) & 1u)
            iwdg_kick();
        REG(F1_FLASH + 0x10) = 0;
    }
}

static void put(uint32_t off, uint32_t val)
{
    if (g_family != FAM_UNKNOWN)
        flash_write_word(g_family, REPORT_ADDR + off, val);
}

static void put_block(uint32_t off, uint32_t src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4)
        put(off + i, REG(src + i));
}

/* ------------------------------------------------------------------------ */
/* Self-erase: must run from RAM because it erases the code it came from.   */

__attribute__((section(".ramfunc"), noinline, noreturn, used))
void self_erase_and_reset(void)
{
    uint32_t fam = g_family;
    __asm volatile("cpsid i");
    if (fam == FAM_F2F4) {
        if (REG(F4_FLASH + 0x10) & (1u << 31)) {
            REG(F4_FLASH + 0x04) = FLASH_KEY1;
            REG(F4_FLASH + 0x04) = FLASH_KEY2;
        }
        REG(F4_FLASH + 0x0C) = 0xF3u;
        /* sector 1 = 0x08004000..0x08007FFF */
        REG(F4_FLASH + 0x10) = (2u << 8) | (1u << 3) | (1u << 1); /* PSIZE x32, SNB=1, SER */
        REG(F4_FLASH + 0x10) |= (1u << 16);                        /* STRT */
        while (REG(F4_FLASH + 0x0C) & (1u << 16))
            REG(0x40003000u) = 0xAAAAu;
        REG(F4_FLASH + 0x10) = 0;
    } else if (fam == FAM_F1 || fam == FAM_F0F3) {
        if (REG(F1_FLASH + 0x10) & (1u << 7)) {
            REG(F1_FLASH + 0x04) = FLASH_KEY1;
            REG(F1_FLASH + 0x04) = FLASH_KEY2;
        }
        REG(F1_FLASH + 0x0C) = 0x34u;
        REG(F1_FLASH + 0x10) = (1u << 1);      /* PER */
        REG(F1_FLASH + 0x14) = APP_BASE;       /* AR */
        REG(F1_FLASH + 0x10) = (1u << 1) | (1u << 6); /* PER | STRT */
        while (REG(F1_FLASH + 0x0C) & 1u)
            REG(0x40003000u) = 0xAAAAu;
        REG(F1_FLASH + 0x10) = 0;
    }
    __asm volatile("dsb");
    REG(0xE000ED0Cu) = 0x05FA0004u; /* AIRCR: SYSRESETREQ */
    for (;;)
        ;
}

/* ------------------------------------------------------------------------ */

static uint32_t detect_family(uint32_t cpuid, uint32_t *idcode)
{
    uint32_t part = (cpuid >> 4) & 0xFFFu;
    uint32_t id;

    if (part == 0xC20u || part == 0xC60u) {  /* Cortex-M0 / M0+ */
        id = REG(0x40015800u);
        *idcode = id;
        id &= 0xFFFu;
        if (id == 0x440 || id == 0x442 || id == 0x444 || id == 0x445 || id == 0x448)
            return FAM_F0F3;
        return FAM_UNKNOWN;
    }

    id = REG(0xE0042000u);
    *idcode = id;
    id &= 0xFFFu;
    switch (id) {
    case 0x410: case 0x412: case 0x414: case 0x418: case 0x420: case 0x428: case 0x430:
        return FAM_F1;
    case 0x422: case 0x432: case 0x438: case 0x439: case 0x446:
        return FAM_F0F3;
    case 0x411: case 0x413: case 0x419: case 0x421: case 0x423: case 0x431:
    case 0x433: case 0x434: case 0x441: case 0x458: case 0x463:
        return FAM_F2F4;
    case 0x000:
        /* STM32F1 erratum: DBGMCU_IDCODE reads 0 without a debugger. */
        if (part == 0xC23u)
            return FAM_F1;
        return FAM_UNKNOWN;
    default:
        return FAM_UNKNOWN;
    }
}

__attribute__((noreturn))
void Reset_Handler(void)
{
    uint32_t sp;
    __asm volatile("cpsid i");
    __asm volatile("mov %0, sp" : "=r"(sp));

    /* Stop anything the bootloader left running. */
    REG(0xE000E010u) = 0;            /* SysTick off */
    REG(0xE000E180u) = 0xFFFFFFFFu;  /* NVIC ICER0 */
    REG(0xE000E280u) = 0xFFFFFFFFu;  /* NVIC ICPR0 */

    uint32_t cpuid = REG(0xE000ED00u);
    uint32_t part = (cpuid >> 4) & 0xFFFu;
    if (part != 0xC20u) {            /* VTOR exists on M0+/M3/M4 */
        REG(0xE000E184u) = 0xFFFFFFFFu;
        REG(0xE000E284u) = 0xFFFFFFFFu;
        REG(0xE000ED08u) = APP_BASE;
    }
    if (part == 0xC23u || part == 0xC24u)
        REG(0xE000ED94u) = 0;        /* MPU off, in case the bootloader enabled it */

    /* .data / .bss (contains the RAM copy of self_erase_and_reset) */
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata)
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;)
        *dst++ = 0;

    g_sp_entry = sp;
    uint32_t idcode = 0;
    uint32_t fam = detect_family(cpuid, &idcode);
    g_family = fam;

    if (fam == FAM_UNKNOWN)
        self_erase_and_reset(); /* cannot write flash safely: just reset */

    flash_unlock(fam);

    uint32_t flash_kb, uid, opt, rcc, gpio, gpio_step, gpio_len;
    if (fam == FAM_F2F4) {
        flash_kb = REG16(0x1FFF7A22u); uid = 0x1FFF7A10u; opt = 0x1FFFC000u;
        rcc = 0x40023800u; gpio = 0x40020000u; gpio_step = 0x400u; gpio_len = 0x28u;
    } else if (fam == FAM_F1) {
        flash_kb = REG16(0x1FFFF7E0u); uid = 0x1FFFF7E8u; opt = 0x1FFFF800u;
        rcc = 0x40021000u; gpio = 0x40010800u; gpio_step = 0x400u; gpio_len = 0x1Cu;
    } else {
        flash_kb = REG16(0x1FFFF7CCu); uid = 0x1FFFF7ACu; opt = 0x1FFFF800u;
        rcc = 0x40021000u; gpio = 0x48000000u; gpio_step = 0x400u; gpio_len = 0x2Cu;
    }

    put(R_HDR + 0x00, MAGIC);
    put(R_HDR + 0x04, 1);           /* report version */
    put(R_HDR + 0x08, fam);
    put(R_HDR + 0x0C, cpuid);
    put(R_HDR + 0x10, idcode);
    put(R_HDR + 0x14, flash_kb);
    put_block(R_HDR + 0x18, uid, 12);

    put(R_STATE + 0x00, sp);
    put(R_STATE + 0x04, part != 0xC20u ? REG(0xE000ED08u) : 0);
    put(R_STATE + 0x08, REG(0xE000E100u)); /* ISER0 (state after our ICER) */
    put(R_STATE + 0x0C, REG(0xE000E010u));

    put_block(R_OPT, opt, 0x20);
    put_block(R_RCC, rcc, 0x100);
    uint32_t nports = (fam == FAM_F0F3) ? 6 : 8;
    for (uint32_t p = 0; p < nports; p++)
        put_block(R_GPIO + p * 0x30, gpio + p * gpio_step, gpio_len);
    if (fam == FAM_F1)
        put_block(R_AFIO, 0x40010000u, 0x20);

    put_block(R_BOOT, BOOT_BASE, BOOT_SIZE);
    put(R_DONE, DONE);

    self_erase_and_reset();
}

void Fault_Handler(void)
{
    self_erase_and_reset();
}

/* Vector table: every exception/IRQ goes to the self-erase path. */
__attribute__((section(".isr_vector"), used))
const void *const vectors[16 + 32] = {
    [0] = &_estack,
    [1] = (void *)Reset_Handler,
    [2 ... 47] = (void *)Fault_Handler,
};
