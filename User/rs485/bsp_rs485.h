#ifndef __BSP_RS485_H
#define __BSP_RS485_H

/**
  ******************************************************************************
  * @file    bsp_rs485.h
  * @brief   USART2 RS485，引脚与收发时序对齐 User485ok 已调通工程。
  *
  * 霸道 V2 已验证：
  *   PA2  USART2_TX
  *   PA3  USART2_RX
  *   PC2  DE/RE（官方例程主控脚）
  *   PD11 DE/RE（部分板子走这脚）
  * 两脚同时驱动，避免只拉其中一只导致发不出去。
  ******************************************************************************
  */

#include "stm32f1xx.h"
#include <stdint.h>

#define RS485_USART                     USART2
#define RS485_USART_IRQn                USART2_IRQn
#define RS485_USART_CLK_ENABLE()        __HAL_RCC_USART2_CLK_ENABLE()
#define RS485_GPIO_CLK_ENABLE()         do { \
                                            __HAL_RCC_GPIOA_CLK_ENABLE(); \
                                            __HAL_RCC_GPIOC_CLK_ENABLE(); \
                                            __HAL_RCC_GPIOD_CLK_ENABLE(); \
                                        } while (0)

#define RS485_TX_PORT                   GPIOA
#define RS485_TX_PIN                    GPIO_PIN_2
#define RS485_RX_PORT                   GPIOA
#define RS485_RX_PIN                    GPIO_PIN_3

#define RS485_DE_ENABLE                 1
#define RS485_DE_PORT                   GPIOC
#define RS485_DE_PIN                    GPIO_PIN_2
#define RS485_DE_PORT_ALT               GPIOD
#define RS485_DE_PIN_ALT                GPIO_PIN_11

#define RS485_BAUDRATE                  9600u
#define RS485_RX_BUF_LEN                256u
#define RS485_IFG_MS                    5u

void            RS485_Init      ( void );
void            RS485_Send      ( const uint8_t *data, uint16_t len );
void            RS485_RxReset   ( void );
uint8_t         RS485_RxReady   ( void );
void            RS485_RxAck     ( void );
uint16_t        RS485_RxLen     ( void );
const uint8_t  *RS485_RxBuf     ( void );

#endif /* __BSP_RS485_H */
