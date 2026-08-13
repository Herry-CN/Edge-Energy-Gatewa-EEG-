/**
  ******************************************************************************
  * @file    GPIO/GPIO_EXTI/Src/stm32f4xx_it.c 
  * @author  MCD Application Team
  * @brief   Main Interrupt Service Routines.
  *          This file provides template for all exceptions handler and 
  *          peripherals interrupt service routine.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; COPYRIGHT(c) 2017 STMicroelectronics</center></h2>
  *
  * Redistribution and use in source and binary forms, with or without modification,
  * are permitted provided that the following conditions are met:
  *   1. Redistributions of source code must retain the above copyright notice,
  *      this list of conditions and the following disclaimer.
  *   2. Redistributions in binary form must reproduce the above copyright notice,
  *      this list of conditions and the following disclaimer in the documentation
  *      and/or other materials provided with the distribution.
  *   3. Neither the name of STMicroelectronics nor the names of its contributors
  *      may be used to endorse or promote products derived from this software
  *      without specific prior written permission.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f1xx_it.h"
#include "./usart/bsp_debug_usart.h"
#include "./common/common.h"
#include "./ESP8266/bsp_esp8266_test.h"
#include "./ESP8266/bsp_esp8266.h"
#include "./ESP8266/bsp_esp8266_mqtt.h"
#include <string.h>


uint16_t publish_task_time=0;//���������ʱ����ֵ
extern uint8_t publish_flag;//���������־

    
/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/******************************************************************************/
/*            Cortex-M7 Processor Exceptions Handlers                         */
/******************************************************************************/

/**
  * @brief   This function handles NMI exception.
  * @param  None
  * @retval None
  */
void NMI_Handler(void)
{
}

/**
  * @brief  This function handles Hard Fault exception.
  * @param  None
  * @retval None
  */
void HardFault_Handler(void)
{
  /* Go to infinite loop when Hard Fault exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles Memory Manage exception.
  * @param  None
  * @retval None
  */
void MemManage_Handler(void)
{
  /* Go to infinite loop when Memory Manage exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles Bus Fault exception.
  * @param  None
  * @retval None
  */
void BusFault_Handler(void)
{
  /* Go to infinite loop when Bus Fault exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles Usage Fault exception.
  * @param  None
  * @retval None
  */
void UsageFault_Handler(void)
{
  /* Go to infinite loop when Usage Fault exception occurs */
  while (1)
  {
  }
}

/**
  * @brief  This function handles SVCall exception.
  * @param  None
  * @retval None
  */
void SVC_Handler(void)
{
}

/**
  * @brief  This function handles Debug Monitor exception.
  * @param  None
  * @retval None
  */
void DebugMon_Handler(void)
{
}

/**
  * @brief  This function handles PendSVC exception.
  * @param  None
  * @retval None
  */
void PendSV_Handler(void)
{
}

/**
  * @brief  This function handles SysTick Handler.
  * @param  None
  * @retval None
  */
void SysTick_Handler(void)
{
    HAL_IncTick();
    if(mqtt_flag == 1)//mqtt������
    {
        publish_task_time++;
        if (publish_task_time == 5000)   //DHT11�ɼ���Ҫ�������2��
        {
            publish_task_time=0;
            publish_flag =1;
        }   
    }
}
  

/**
  * @brief  ����1�жϷ�����
  * @param  None
  * @retval None
  */
void DEBUG_USART_IRQHandler(void)
{
  uint8_t ucCh;
	if ( __HAL_USART_GET_FLAG ( &UartHandle, USART_FLAG_RXNE ) != RESET )
	{
		 HAL_UART_Receive( &UartHandle,(uint8_t *)&ucCh, 1, 1000 );
		
		if ( strUSART_Fram_Record .InfBit .FramLength < ( RX_BUF_MAX_LEN - 1 ) )                       //Ԥ��1���ֽ�д������
			   strUSART_Fram_Record .Data_RX_BUF [ strUSART_Fram_Record .InfBit .FramLength ++ ]  = ucCh;
        __HAL_UART_CLEAR_FLAG( &UartHandle, USART_FLAG_RXNE );
	}
	 	 
	if (__HAL_USART_GET_FLAG ( &UartHandle, USART_FLAG_IDLE ) == SET )                                         //����֡�������
	{
        strUSART_Fram_Record .InfBit .FramFinishFlag = 1;		
		
		 HAL_UART_Receive( &UartHandle,(uint8_t *)&ucCh, 1, 1000 );                  //��������������жϱ�־λ(�ȶ�USART_SR��Ȼ���USART_DR)	
         __HAL_UART_CLEAR_IDLEFLAG(&UartHandle);
    }
}

/**
  * @brief  This function handles macESP8266_USARTx Handler.
  * @param  None
  * @retval None
  */
void macESP8266_USART_INT_FUN ( void )
{	
    char ucCh;
    if ( __HAL_USART_GET_FLAG( &Uart3Handle, USART_FLAG_RXNE ) != RESET )
    {
        HAL_UART_Receive( &Uart3Handle,(uint8_t *)&ucCh, 1, 1000 );
        
        if ( strEsp8266_Fram_Record .InfBit .FramLength < ( RX_BUF_MAX_LEN - 1 ) )                       //Ԥ��1���ֽ�д������
            strEsp8266_Fram_Record .Data_RX_BUF [ strEsp8266_Fram_Record .InfBit .FramLength ++ ]  = ucCh;
        __HAL_UART_CLEAR_FLAG( &Uart3Handle, USART_FLAG_RXNE );
    }
        
    if ( __HAL_USART_GET_FLAG( &Uart3Handle, USART_FLAG_IDLE ) == SET )                                         //����֡�������
    {
        strEsp8266_Fram_Record .InfBit .FramFinishFlag = 1;
        strEsp8266_Fram_Record .Data_RX_BUF [ strEsp8266_Fram_Record .InfBit .FramLength ] = '\0';

        ucTcpClosedFlag = strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "CLOSED\r\n" ) ? 1 : 0;
        if(mqtt_flag ==1 )//mqtt��������
        {
            if (!g_mqtt_rx_pending &&
                strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTSUBRECV") &&
                strstr(strEsp8266_Fram_Record.Data_RX_BUF, "\"type\":\"command\"") &&
                strstr(strEsp8266_Fram_Record.Data_RX_BUF, "\"name\":\"onoff\""))
            {
                uint16_t copy_len = strEsp8266_Fram_Record.InfBit.FramLength;
                if (copy_len >= RX_BUF_MAX_LEN)
                {
                    copy_len = RX_BUF_MAX_LEN - 1;
                }
                memcpy(g_mqtt_rx_frame, strEsp8266_Fram_Record.Data_RX_BUF, copy_len);
                g_mqtt_rx_frame[copy_len] = '\0';
                g_mqtt_rx_len = copy_len;
                g_mqtt_rx_pending = 1;
            }
        }
        __HAL_UART_CLEAR_IDLEFLAG(&Uart3Handle);
    }	

}

/******************************************************************************/
/*                 STM32F7xx Peripherals Interrupt Handlers                   */
/*  Add here the Interrupt Handler for the used peripheral(s) (PPP), for the  */
/*  available peripheral interrupt handler's name please refer to the startup */
/*  file (startup_stm32f7xx.s).                                               */
/******************************************************************************/

/**
  * @brief  This function handles PPP interrupt request.
  * @param  None
  * @retval None
  */
/*void PPP_IRQHandler(void)
{
}*/


/**
  * @}
  */ 

/**
  * @}
  */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
