#ifndef STM32F103_H
#define STM32F103_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define REG16(addr) (*(volatile uint16_t *)(addr))

/* Core Cortex-M3 */
#define SCB_VTOR        REG32(0xE000ED08u)
#define SCB_AIRCR       REG32(0xE000ED0Cu)
#define SYST_CSR        REG32(0xE000E010u)
#define SYST_RVR        REG32(0xE000E014u)
#define SYST_CVR        REG32(0xE000E018u)
#define NVIC_ISER0      REG32(0xE000E100u)
#define NVIC_ISER1      REG32(0xE000E104u)
#define NVIC_ICER0      REG32(0xE000E180u)
#define NVIC_ICER1      REG32(0xE000E184u)

/* RCC */
#define RCC_BASE        0x40021000u
#define RCC_CR          REG32(RCC_BASE + 0x00u)
#define RCC_CFGR        REG32(RCC_BASE + 0x04u)
#define RCC_CIR         REG32(RCC_BASE + 0x08u)
#define RCC_APB2RSTR    REG32(RCC_BASE + 0x0Cu)
#define RCC_APB1RSTR    REG32(RCC_BASE + 0x10u)
#define RCC_AHBENR      REG32(RCC_BASE + 0x14u)
#define RCC_APB2ENR     REG32(RCC_BASE + 0x18u)
#define RCC_APB1ENR     REG32(RCC_BASE + 0x1Cu)

/* FLASH */
#define FLASH_ACR       REG32(0x40022000u)

/* PWR & BKP */
#define PWR_CR          REG32(0x40007000u)
#define BKP_DR1         REG32(0x40006C04u)

/* AFIO */
#define AFIO_MAPR       REG32(0x40010004u)

/* GPIO */
typedef struct {
    volatile uint32_t CRL;
    volatile uint32_t CRH;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t BRR;
    volatile uint32_t LCKR;
} GPIO_TypeDef;

#define GPIOA ((GPIO_TypeDef *)0x40010800u)
#define GPIOB ((GPIO_TypeDef *)0x40010C00u)
#define GPIOC ((GPIO_TypeDef *)0x40011000u)

/* GPIO pin mode helpers (STM32F1 CNF[1:0] | MODE[1:0]) */
#define GPIO_MODE_ANALOG    0x0u
#define GPIO_MODE_IN_FLOAT  0x4u
#define GPIO_MODE_IN_PUPD   0x8u
#define GPIO_MODE_OUT_PP_50 0x3u
#define GPIO_MODE_OUT_PP_10 0x1u
#define GPIO_MODE_AF_PP_50  0xBu
#define GPIO_MODE_AF_PP_10  0x9u
#define GPIO_MODE_AF_OD_50  0xFu

static inline void gpio_config(GPIO_TypeDef *port, uint8_t pin, uint8_t mode) {
    if (pin < 8) {
        uint32_t shift = pin * 4u;
        port->CRL = (port->CRL & ~(0xFu << shift)) | ((uint32_t)mode << shift);
    } else {
        uint32_t shift = (pin - 8u) * 4u;
        port->CRH = (port->CRH & ~(0xFu << shift)) | ((uint32_t)mode << shift);
    }
}

static inline void gpio_set(GPIO_TypeDef *port, uint8_t pin)   { port->BSRR = 1u << pin; }
static inline void gpio_clear(GPIO_TypeDef *port, uint8_t pin) { port->BRR  = 1u << pin; }
static inline uint8_t gpio_read(GPIO_TypeDef *port, uint8_t pin) { return (port->IDR >> pin) & 1u; }

/* SPI */
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t CRCPR;
    volatile uint32_t RXCRCR;
    volatile uint32_t TXCRCR;
} SPI_TypeDef;

#define SPI1 ((SPI_TypeDef *)0x40013000u)
#define SPI2 ((SPI_TypeDef *)0x40003800u)

/* TIM */
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMCR;
    volatile uint32_t DIER;
    volatile uint32_t SR;
    volatile uint32_t EGR;
    volatile uint32_t CCMR1;
    volatile uint32_t CCMR2;
    volatile uint32_t CCER;
    volatile uint32_t CNT;
    volatile uint32_t PSC;
    volatile uint32_t ARR;
    volatile uint32_t RCR;
    volatile uint32_t CCR1;
    volatile uint32_t CCR2;
    volatile uint32_t CCR3;
    volatile uint32_t CCR4;
} TIM_TypeDef;

#define TIM3 ((TIM_TypeDef *)0x40000400u)

/* I2C */
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t OAR1;
    volatile uint32_t OAR2;
    volatile uint32_t DR;
    volatile uint32_t SR1;
    volatile uint32_t SR2;
    volatile uint32_t CCR;
    volatile uint32_t TRISE;
} I2C_TypeDef;

#define I2C1 ((I2C_TypeDef *)0x40005400u)

/* ADC1 */
typedef struct {
    volatile uint32_t SR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMPR1;
    volatile uint32_t SMPR2;
    volatile uint32_t JOFR1;
    volatile uint32_t JOFR2;
    volatile uint32_t JOFR3;
    volatile uint32_t JOFR4;
    volatile uint32_t HTR;
    volatile uint32_t LTR;
    volatile uint32_t SQR1;
    volatile uint32_t SQR2;
    volatile uint32_t SQR3;
} ADC_TypeDef;

#define ADC1 ((ADC_TypeDef *)0x40012400u)
#define ADC1_DR REG32(0x4001244Cu)

/* USB Full-Speed device (STM32F103) */
#define USB_BASE        0x40005C00u
#define USB_EP(n)       REG32(USB_BASE + (n) * 4u)
#define USB_CNTR        REG32(USB_BASE + 0x40u)
#define USB_ISTR        REG32(USB_BASE + 0x44u)
#define USB_FNR         REG32(USB_BASE + 0x48u)
#define USB_DADDR       REG32(USB_BASE + 0x4Cu)
#define USB_BTABLE      REG32(USB_BASE + 0x50u)
#define USB_PMA_BASE    0x40006000u

/* Timing (provided by startup.c) */
extern volatile uint32_t g_ms;
void delay_ms(uint32_t ms);

/* Reboot into Displio USB HID bootloader */
void enter_bootloader(void);

#endif
