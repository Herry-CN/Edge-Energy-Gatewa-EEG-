/**
  ******************************************************************************
  * @file    main.c
  * @author  fire
  * @version V1.0
  * @date    2024-xx-xx
  * @brief   
  ******************************************************************************
  * @attention
  *
  * ʵ��ƽ̨:Ұ�� STM32F103 ������ 
  * ��̳    :http://www.firebbs.cn
  * �Ա�    :https://fire-stm32.taobao.com
  *
  ******************************************************************************
  */ 

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f1xx.h"
#include "./usart/bsp_debug_usart.h"
#include <stdlib.h>
#include "./led/bsp_led.h" 
#include "./dht11/bsp_dht11.h"
#include "./common/common.h"
#include "./ESP8266/bsp_esp8266_test.h"
#include "./ESP8266/bsp_esp8266.h"
#include "./systick/bsp_SysTick.h"
#include "./ESP8266/bsp_esp8266_mqtt.h"
#include "./wdg/bsp_iwdg.h"


uint8_t publish_flag =0;//���������־

/**
  * @brief  ������
  * @param  ��
  * @retval ��
  */
int main(void)
{   
    /* Init SysTick for HAL_Delay */
    SysTick_Init();
    /* SystemCoreClock = 72 MHz via HSE+PLL x9 */
    SystemClock_Config();
    /* USART1 @ 115200 8-N-1 for debug printf */
    DEBUG_USART_Config();
    printf("\r\n--- Charging Pile MQTT Demo (STM32F103ZE + ESP8266 MQTT-AT 1MB) ---\r\n"
             "Edit ESP8266/bsp_esp8266_test.h to change: WiFi SSID, device ID, MQTT broker.\r\n");
    /* Arm the watchdog before the (blocking) startup chain, and report why the
     * board came up. An "IWDG WATCHDOG TIMEOUT" line in the serial log is the
     * quickest way to tell a real lock-up from a power glitch. */
    IWDG_Config();
    printf("[BOOT] reset reason: %s\r\n", IWDG_GetResetReason());
    /* LED GPIO init */
    LED_GPIO_Config();
    /* DHT11 temp/humidity sensor init */
    DHT11_Init();
    /* ESP8266 USART3 + CH_PD/RST GPIOs init */
    ESP8266_Init();
    /* Full startup chain (AT -> WiFi -> MQTT USERCFG/CONN/SUB). Blocks until OK. */
    ESP8266_StaTcpClient_Unvarnish_ConfigTest();
    
    while (1)
    {
        /* Kick the watchdog once per iteration. Everything that can block for
         * longer than a loop pass (ESP8266_Cmd) feeds it internally as well. */
        IWDG_Feed();
        /* Always poll business service:
         * - handles MQTT downlink pending flag immediately
         * - handles periodic status publish only when publish_flag is set */
        ESP8266_SendDHT11DataTest();
    }   
}


/**
  * @brief  System Clock Configuration
  *         The system Clock is configured as follow : 
  *            System Clock source            = PLL (HSE)
  *            SYSCLK(Hz)                     = 72000000
  *            HCLK(Hz)                       = 72000000
  *            AHB Prescaler                  = 1
  *            APB1 Prescaler                 = 2
  *            APB2 Prescaler                 = 1
  *            HSE Frequency(Hz)              = 8000000
  *            HSE PREDIV1                    = 1
  *            PLLMUL                         = 9
  *            Flash Latency(WS)              = 2
  * @param  None
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_ClkInitTypeDef clkinitstruct = {0};
  RCC_OscInitTypeDef oscinitstruct = {0};
  
  /* Enable HSE Oscillator and activate PLL with HSE as source */
  oscinitstruct.OscillatorType  = RCC_OSCILLATORTYPE_HSE;
  oscinitstruct.HSEState        = RCC_HSE_ON;
  oscinitstruct.HSEPredivValue  = RCC_HSE_PREDIV_DIV1;
  oscinitstruct.PLL.PLLState    = RCC_PLL_ON;
  oscinitstruct.PLL.PLLSource   = RCC_PLLSOURCE_HSE;
  oscinitstruct.PLL.PLLMUL      = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&oscinitstruct)!= HAL_OK)
  {
    /* Initialization Error */
    while(1); 
  }

  /* Select PLL as system clock source and configure the HCLK, PCLK1 and PCLK2 
     clocks dividers */
  clkinitstruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2);
  clkinitstruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clkinitstruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clkinitstruct.APB2CLKDivider = RCC_HCLK_DIV1;
  clkinitstruct.APB1CLKDivider = RCC_HCLK_DIV2;  
  if (HAL_RCC_ClockConfig(&clkinitstruct, FLASH_LATENCY_2)!= HAL_OK)
  {
    /* Initialization Error */
    while(1); 
  }
}

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
