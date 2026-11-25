#include "stm32f103.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);
void USB_LP_IRQHandler(void);

volatile uint32_t g_ms = 0;

void *memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void SysTick_Handler(void) {
    g_ms++;
}

void delay_ms(uint32_t ms) {
    uint32_t start = g_ms;
    while ((uint32_t)(g_ms - start) < ms) {
        __asm volatile("nop");
    }
}

void enter_bootloader(void) {
    /* Disconnect USB D+ pull-up (PA10 HIGH) */
    gpio_set(GPIOA, 10);
    delay_ms(20);
    /* Write non-zero magic to BKP_DR1 so bootloader stays in USB HID mode */
    RCC_APB1ENR |= (1u << 28) | (1u << 27); /* PWREN | BKPEN */
    PWR_CR |= (1u << 8);                    /* DBP: allow backup domain write */
    BKP_DR1 = 0xB007u;
    __asm volatile("dsb");
    SCB_AIRCR = 0x05FA0004u;                /* SYSRESETREQ */
    for (;;) {}
}

static void clock_init_72mhz(void) {
    /* Enable HSE (8 MHz crystal) */
    RCC_CR |= (1u << 16);
    while (!(RCC_CR & (1u << 17))) {}

    /* Flash 2 wait states + prefetch buffer for 72 MHz */
    FLASH_ACR = 0x12u;

    /* Reset CFGR if PLL already running (bootloader leaves PLL on!) */
    if ((RCC_CFGR & 0x0Cu) == 0x08u) {
        /* Switch back to HSE temporarily */
        RCC_CFGR = (RCC_CFGR & ~0x03u) | 0x01u;
        while ((RCC_CFGR & 0x0Cu) != 0x04u) {}
    }
    RCC_CR &= ~(1u << 24); /* PLLOFF */
    while (RCC_CR & (1u << 25)) {}

    /* PPRE1 = /2 (36 MHz), PLLSRC = HSE, PLLMUL = x9 (72 MHz), USBPRE = /1.5 (48 MHz) */
    RCC_CFGR = (4u << 8) | (1u << 16) | (7u << 18);

    RCC_CR |= (1u << 24); /* PLLON */
    while (!(RCC_CR & (1u << 25))) {}

    /* Switch SYSCLK to PLL */
    RCC_CFGR = (RCC_CFGR & ~0x03u) | 0x02u;
    while ((RCC_CFGR & 0x0Cu) != 0x08u) {}
}

void Default_Handler(void) {
    /* On unexpected fault, reboot cleanly into bootloader so we never hang! */
    enter_bootloader();
}

__attribute__((noreturn))
void Reset_Handler(void) {
    __asm volatile("cpsid i");
    SYST_CSR = 0;
    NVIC_ICER0 = 0xFFFFFFFFu;
    NVIC_ICER1 = 0xFFFFFFFFu;
    SCB_VTOR = 0x08004000u;

    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;) *dst++ = 0;

    clock_init_72mhz();

    /* Enable AFIO, GPIOA, GPIOB, GPIOC clocks; disable JTAG, keep SWD */
    RCC_APB2ENR |= (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4);
    AFIO_MAPR = (AFIO_MAPR & ~(7u << 24)) | (2u << 24);

    /* Keep power latch PA1 HIGH */
    gpio_config(GPIOA, 1, GPIO_MODE_OUT_PP_50);
    gpio_set(GPIOA, 1);

    /* Configure SysTick for 1 ms tick at 72 MHz */
    SYST_RVR = 72000u - 1u;
    SYST_CVR = 0;
    SYST_CSR = 7u; /* ENABLE | TICKINT | CLKSOURCE */

    __asm volatile("cpsie i");
    main();
    enter_bootloader();
    for (;;) {}
}

__attribute__((section(".isr_vector"), used))
const void *const g_vectors[16 + 43] = {
    [0]  = &_estack,
    [1]  = (void *)Reset_Handler,
    [2]  = (void *)Default_Handler, /* NMI */
    [3]  = (void *)Default_Handler, /* HardFault */
    [4]  = (void *)Default_Handler, /* MemManage */
    [5]  = (void *)Default_Handler, /* BusFault */
    [6]  = (void *)Default_Handler, /* UsageFault */
    [11] = (void *)Default_Handler, /* SVCall */
    [14] = (void *)Default_Handler, /* PendSV */
    [15] = (void *)SysTick_Handler,
    [16 + 20] = (void *)USB_LP_IRQHandler, /* IRQ 20: USB_LP_CAN1_RX0 */
};
