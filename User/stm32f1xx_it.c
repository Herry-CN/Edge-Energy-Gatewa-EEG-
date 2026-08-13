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


uint16_t publish_task_time=0;//���������ʱ�����?
extern uint8_t publish_flag;//����������?

    
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
        if (publish_task_time == 5000)   //DHT11�ɼ���Ҫ�������?��
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
    uint32_t sr = UartHandle.Instance->SR;
    uint8_t  dr_taken = 0;

    /* Same reasoning as the USART3 handler below: never call HAL_UART_Receive()
     * from an ISR. It takes the handle lock that fputc()'s HAL_UART_Transmit()
     * holds for the whole of every printf(), so it would return HAL_BUSY without
     * ever reading DR: RXNE stays set and this ISR re-enters forever.
     * Dormant today - DEBUG_USART_Config() leaves the USART1 RX interrupt off -
     * but it has to be safe the day the debug port starts accepting input. */
    if ( sr & ( USART_SR_RXNE | USART_SR_ORE ) )
    {
        uint8_t  ucCh = ( uint8_t ) ( UartHandle.Instance->DR & 0xFF );
        uint16_t len  = strUSART_Fram_Record .InfBit .FramLength;

        /* Reading SR (above) then DR clears RXNE and ORE in one go, so an
         * overrun cannot wedge the handler either. */
        dr_taken = 1;
		
        if ( ( sr & USART_SR_RXNE ) && ( len < ( RX_BUF_MAX_LEN - 1 ) ) )
        {                       //Ԥ��1���ֽ�д������
            strUSART_Fram_Record .Data_RX_BUF [ len ]     = ( char ) ucCh;
            strUSART_Fram_Record .InfBit .FramLength      = len + 1;
            strUSART_Fram_Record .Data_RX_BUF [ len + 1 ] = '\0';
        }
    }
	 	 
    if ( sr & USART_SR_IDLE )                                         //����֡�������?
	{
        if ( !dr_taken )
        {
            ( void ) UartHandle.Instance->DR;
        }

        strUSART_Fram_Record .InfBit .FramFinishFlag = 1;		
		
        /* IDLE is already cleared by the SR read at the top of the handler
         * plus the DR read above. __HAL_UART_CLEAR_IDLEFLAG() must not be used
         * here: it reads DR a second time and would swallow a byte that
         * arrived in the meantime. */
    }
}

/**
  * @brief  Last occurrence of needle in hay (strstr only finds the first one).
  */
static const char * last_occurrence ( const char * hay, const char * needle )
{
    const char * hit  = NULL;
    const char * scan = strstr ( hay, needle );

    while ( scan != NULL )
    {
        hit  = scan;
        scan = strstr ( scan + 1, needle );
    }

    return hit;
}

/**
  * @brief  This function handles macESP8266_USARTx Handler.
  * @param  None
  * @retval None
  */
void macESP8266_USART_INT_FUN ( void )
{	
    uint32_t sr = Uart3Handle.Instance->SR;
    uint8_t  dr_taken = 0;

    /* Read DR directly instead of calling HAL_UART_Receive(): that HAL call
     * takes the per-handle lock which HAL_UART_Transmit() holds while thread
     * mode is sending, so it returns HAL_BUSY without ever reading DR. RXNE
     * would stay set, this ISR would re-enter immediately and forever, and the
     * transmit it starves could never release the lock.
     * Reading SR (above) and then DR also clears an ORE overrun, which would
     * otherwise block every further byte on this port. */
    if ( sr & ( USART_SR_RXNE | USART_SR_ORE ) )
    {
        uint8_t  ucCh = ( uint8_t ) ( Uart3Handle.Instance->DR & 0xFF );
        uint16_t len  = strEsp8266_Fram_Record .InfBit .FramLength;

        dr_taken = 1;

        if ( sr & USART_SR_ORE )
        {
            g_esp8266_rx_drop++;     /* hardware overrun: at least one byte lost */
        }

        if ( sr & USART_SR_RXNE )
        {
            if ( len < ( RX_BUF_MAX_LEN - 1 ) )
            {
                strEsp8266_Fram_Record .Data_RX_BUF [ len ] = ( char ) ucCh;
                strEsp8266_Fram_Record .InfBit .FramLength  = len + 1;
                /* Keep the buffer terminated after every byte so thread mode
                   can strstr() it without waiting for the IDLE frame gap. */
                strEsp8266_Fram_Record .Data_RX_BUF [ len + 1 ] = '\0';
            }
            else
            {
                g_esp8266_rx_drop++;
            }
        }
    }
        
    if ( sr & USART_SR_IDLE )
    {
        if ( !dr_taken )
        {
            ( void ) Uart3Handle.Instance->DR;   /* SR was read above; reading DR clears IDLE */
        }

        strEsp8266_Fram_Record .InfBit .FramFinishFlag = 1;

        ucTcpClosedFlag = strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "CLOSED\r\n" ) ? 1 : 0;
        if(mqtt_flag ==1 )//mqtt��������
        {
            /* Scan from the LAST +MQTTSUBRECV in the buffer, not the first.
             * The board subscribes to the very topic it publishes replies on,
             * so an echo of our own reply is regularly still sitting in the
             * buffer. Parsing from the first URC would read "type":"reply" and
             * silently drop the real command that arrived behind it. */
            const char * urc = last_occurrence ( strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTSUBRECV" );

            if (!g_mqtt_rx_pending && ( urc != NULL ) &&
                strstr(urc, "\"type\":\"command\"") &&
                strstr(urc, "\"name\":\"onoff\""))
            {
                uint16_t offset   = (uint16_t)( urc - strEsp8266_Fram_Record.Data_RX_BUF );
                uint16_t copy_len = strEsp8266_Fram_Record.InfBit.FramLength - offset;
                if (copy_len >= RX_BUF_MAX_LEN)
                {
                    copy_len = RX_BUF_MAX_LEN - 1;
                }
                memcpy(g_mqtt_rx_frame, urc, copy_len);
                g_mqtt_rx_frame[copy_len] = '\0';
                g_mqtt_rx_len = copy_len;
                g_mqtt_rx_pending = 1;
            }
        }
        /* Nobody is waiting for this data and the buffer is nearly full: drop
           it now. Otherwise a burst of unsolicited URCs between two commands
           fills the 2 KB buffer and every later byte is lost until some AT
           command happens to reset it. */
        if ( !ESP8266_AT_CmdInFlight() &&
             ( strEsp8266_Fram_Record .InfBit .FramLength >= ESP8266_RX_HIGH_WATER ) )
        {
            strEsp8266_Fram_Record .InfBit .FramLength     = 0;
            strEsp8266_Fram_Record .InfBit .FramFinishFlag = 0;
            strEsp8266_Fram_Record .Data_RX_BUF [ 0 ]      = '\0';
        }
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
