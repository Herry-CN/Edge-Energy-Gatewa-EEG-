/**
  ******************************************************************************
  * @file    stm32f1xx_it_bridge.c  (UART Bridge v2)
  * @brief   中断服务函数 - 串口桥接模式
  *          USART1 收到数据 -> 立即转发到 USART3（PC -> ESP8266）
  *          USART3 收到数据 -> 立即转发到 USART1（ESP8266 -> PC）
  *
  * v2 修复：
  *   ★ Bug 1: IDLE 标志清除顺序错误
  *     原代码先读 DR 再读 SR，STM32F1xx 要求先读 SR 再读 DR 才能清除 IDLE
  *     → IDLE 标志可能无法清除，导致反复触发空闲中断，淹没正常接收
  *
  *   ★ Bug 2: 未处理 ORE（溢出错误）
  *     烧录时数据是突发传输，如果 ISR 里 while(TXE) 阻塞期间又来字节，
  *     ORE 置位后接收器完全停摆，后续所有数据丢失 → 烧录必败
  *
  *   ★ Bug 3: ISR 中 TXE 忙等无超时
  *     如果目标串口 TX 异常（短路/断线），while(TXE) 会永久死循环，
  *     整个系统中断瘫痪。加入超时跳出保护。
  *
  *   ★ Bug 4: NE/FE 错误标志未清除
  *     噪声错误和帧错误标志累积也会影响接收，需要清除。
  *
  *  ⚠️ 烧录 ESP8266 完成后，请恢复原始的 stm32f1xx_it.c 文件！
  ******************************************************************************
  */

#include "main.h"
#include "stm32f1xx_it.h"
#include "./usart/bsp_debug_usart.h"
#include "./ESP8266/bsp_esp8266.h"

extern UART_HandleTypeDef UartHandle;   /* USART1 - CH340/USB */
extern UART_HandleTypeDef Uart3Handle;  /* USART3 - ESP8266 */

/* TXE 忙等超时计数上限（约 1ms @72MHz，够 115200 发一个字节了） */
#define BRIDGE_TXE_TIMEOUT  5000U

/******************************************************************************/
/*            Cortex-M3 Processor Exceptions Handlers                         */
/******************************************************************************/

void NMI_Handler(void) {}
void HardFault_Handler(void) { while (1) {} }
void MemManage_Handler(void) { while (1) {} }
void BusFault_Handler(void) { while (1) {} }
void UsageFault_Handler(void) { while (1) {} }
void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}

/**
  * @brief  SysTick Handler - 仅递增 HAL 计数器（HAL_GetTick/HAL_Delay 依赖）
  */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/******************************************************************************/
/*            USART1 中断 - PC -> ESP8266 转发                                 */
/******************************************************************************/

/**
  * @brief  USART1 中断处理
  *         从 CH340/USB 收到的每个字节，立即写入 USART3 发给 ESP8266
  */
void DEBUG_USART_IRQHandler(void)
{
    uint8_t ch;
    uint32_t timeout;

    /* ---- ORE 溢出错误处理（必须最先处理） ----
     * ORE 置位时，读 SR 再读 DR 可清除，否则接收器停摆 */
    if (__HAL_USART_GET_FLAG(&UartHandle, USART_FLAG_ORE) != RESET)
    {
        (void)UartHandle.Instance->SR;
        (void)UartHandle.Instance->DR;  /* 读 DR 清除 ORE */
    }

    /* ---- NE 噪声错误 / FE 帧错误 清除 ---- */
    if (__HAL_USART_GET_FLAG(&UartHandle, USART_FLAG_NE) != RESET)
    {
        (void)UartHandle.Instance->SR;
        (void)UartHandle.Instance->DR;
    }
    if (__HAL_USART_GET_FLAG(&UartHandle, USART_FLAG_FE) != RESET)
    {
        (void)UartHandle.Instance->SR;
        (void)UartHandle.Instance->DR;
    }

    /* ---- 接收数据寄存器非空（正常接收） ---- */
    if (__HAL_USART_GET_FLAG(&UartHandle, USART_FLAG_RXNE) != RESET)
    {
        /* 直接读 DR 寄存器获取字节（同时清除 RXNE 标志） */
        ch = (uint8_t)(UartHandle.Instance->DR & 0xFF);

        /* 等待 USART3 发送数据寄存器为空，带超时保护 */
        timeout = BRIDGE_TXE_TIMEOUT;
        while (!(Uart3Handle.Instance->SR & USART_FLAG_TXE) && --timeout);

        /* 超时未到才写入（超时说明 USART3 TX 异常，丢弃此字节） */
        if (timeout)
        {
            Uart3Handle.Instance->DR = (uint32_t)ch;
        }
    }

    /* ---- 总线空闲中断清除 ----
     * ★修复：STM32F1xx 要求先读 SR 再读 DR 才能清除 IDLE 标志
     *   原代码顺序反了（先 DR 后 SR），IDLE 可能无法清除 */
    if (__HAL_USART_GET_FLAG(&UartHandle, USART_FLAG_IDLE) == SET)
    {
        (void)UartHandle.Instance->SR;   /* 先读 SR */
        (void)UartHandle.Instance->DR;   /* 再读 DR */
    }
}

/******************************************************************************/
/*            USART3 中断 - ESP8266 -> PC 转发                                 */
/******************************************************************************/

/**
  * @brief  USART3 中断处理
  *         从 ESP8266 收到的每个字节，立即写入 USART1 发给 PC
  */
void macESP8266_USART_INT_FUN(void)
{
    uint8_t ch;
    uint32_t timeout;

    /* ---- ORE 溢出错误处理 ---- */
    if (__HAL_USART_GET_FLAG(&Uart3Handle, USART_FLAG_ORE) != RESET)
    {
        (void)Uart3Handle.Instance->SR;
        (void)Uart3Handle.Instance->DR;
    }

    /* ---- NE 噪声错误 / FE 帧错误 清除 ---- */
    if (__HAL_USART_GET_FLAG(&Uart3Handle, USART_FLAG_NE) != RESET)
    {
        (void)Uart3Handle.Instance->SR;
        (void)Uart3Handle.Instance->DR;
    }
    if (__HAL_USART_GET_FLAG(&Uart3Handle, USART_FLAG_FE) != RESET)
    {
        (void)Uart3Handle.Instance->SR;
        (void)Uart3Handle.Instance->DR;
    }

    /* ---- 接收数据寄存器非空（正常接收） ---- */
    if (__HAL_USART_GET_FLAG(&Uart3Handle, USART_FLAG_RXNE) != RESET)
    {
        /* 直接读 DR 寄存器获取字节（同时清除 RXNE 标志） */
        ch = (uint8_t)(Uart3Handle.Instance->DR & 0xFF);

        /* 等待 USART1 发送数据寄存器为空，带超时保护 */
        timeout = BRIDGE_TXE_TIMEOUT;
        while (!(UartHandle.Instance->SR & USART_FLAG_TXE) && --timeout);

        /* 超时未到才写入（超时说明 USART1 TX 异常，丢弃此字节） */
        if (timeout)
        {
            UartHandle.Instance->DR = (uint32_t)ch;
        }
    }

    /* ---- 总线空闲中断清除（顺序修正） ---- */
    if (__HAL_USART_GET_FLAG(&Uart3Handle, USART_FLAG_IDLE) == SET)
    {
        (void)Uart3Handle.Instance->SR;   /* 先读 SR */
        (void)Uart3Handle.Instance->DR;   /* 再读 DR */
    }
}

/******************************************************************************/
/*                          End of file                                       */
/******************************************************************************/
