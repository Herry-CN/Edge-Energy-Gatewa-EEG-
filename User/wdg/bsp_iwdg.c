/**
  ******************************************************************************
  * @file    bsp_iwdg.c
  * @brief   Independent watchdog + reset cause reporting.
  ******************************************************************************
  */

#include "./wdg/bsp_iwdg.h"

static IWDG_HandleTypeDef s_iwdg;
static bool               s_iwdg_started  = false;
static bool               s_was_wdg_reset = false;
static const char        *s_reset_reason  = "unknown";

/**
  * @brief  Latch why the board rebooted, then arm the watchdog.
  * @note   Must run after the system clock and before the startup chain.
  */
void IWDG_Config ( void )
{
    /* A watchdog reset also asserts the NRST pin flag, so test IWDGRST first.
       Flags are sticky across resets and are cleared here so that the next
       boot reports its own cause. */
    if ( __HAL_RCC_GET_FLAG ( RCC_FLAG_IWDGRST ) )
    {
        s_was_wdg_reset = true;
        s_reset_reason  = "IWDG WATCHDOG TIMEOUT";
    }
    else if ( __HAL_RCC_GET_FLAG ( RCC_FLAG_SFTRST ) )
    {
        s_reset_reason = "software reset";
    }
    else if ( __HAL_RCC_GET_FLAG ( RCC_FLAG_PORRST ) )
    {
        s_reset_reason = "power-on";
    }
    else if ( __HAL_RCC_GET_FLAG ( RCC_FLAG_PINRST ) )
    {
        s_reset_reason = "NRST pin";
    }
    else if ( __HAL_RCC_GET_FLAG ( RCC_FLAG_LPWRRST ) )
    {
        s_reset_reason = "low-power reset";
    }
    __HAL_RCC_CLEAR_RESET_FLAGS();

    /* Freeze the counter whenever the core is halted, otherwise every
       breakpoint would end the debug session with a watchdog reset. */
    __HAL_DBGMCU_FREEZE_IWDG();

    s_iwdg.Instance       = IWDG;
    s_iwdg.Init.Prescaler = IWDG_PRESCALER_128;
    s_iwdg.Init.Reload    = 0x0FFF;

    /* HAL_IWDG_Init() arms the counter before it writes the prescaler, so the
       watchdog is running even if the register write times out. Always refresh
       from here on rather than keying off the return value. */
    ( void ) HAL_IWDG_Init ( &s_iwdg );
    s_iwdg_started = true;
}

/**
  * @brief  Refresh the watchdog counter.
  */
void IWDG_Feed ( void )
{
    if ( s_iwdg_started )
    {
        HAL_IWDG_Refresh ( &s_iwdg );
    }
}

bool IWDG_WasWatchdogReset ( void )
{
    return s_was_wdg_reset;
}

const char * IWDG_GetResetReason ( void )
{
    return s_reset_reason;
}

/************************ END OF FILE *****************************/
