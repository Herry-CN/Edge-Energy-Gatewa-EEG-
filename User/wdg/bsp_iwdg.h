#ifndef __BSP_IWDG_H
#define __BSP_IWDG_H

#include "stm32f1xx.h"
#include <stdbool.h>

/*
 * Independent watchdog for the gateway.
 *
 * The IWDG runs from the LSI, which STM32F1 only specifies as 30..60 kHz, so
 * the nominal 13.1 s period (prescaler 128, reload 4095, LSI = 40 kHz) really
 * means "somewhere between 8.7 s and 17.5 s". Every blocking section must
 * therefore stay well under 8.7 s between two IWDG_Feed() calls:
 *   - ESP8266_Cmd() feeds while it polls, so even a 5 s AT+CWJAP is covered
 *   - the main loop feeds once per iteration
 *
 * What is deliberately NOT fed: the fatal error loops, HardFault_Handler, and
 * the unbounded DHT11 bit-banging loops. A gateway that lands in one of those
 * reboots and retries instead of sitting there dead until someone notices.
 */

void        IWDG_Config          ( void );   /* arm the watchdog, latch the reset cause */
void        IWDG_Feed            ( void );   /* refresh the counter */
bool        IWDG_WasWatchdogReset( void );   /* true if the previous reset was a timeout */
const char *IWDG_GetResetReason  ( void );   /* human readable cause of the previous reset */

#endif /* __BSP_IWDG_H */
