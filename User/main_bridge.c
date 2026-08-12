/**
  ******************************************************************************
  * @file    main_bridge.c  (UART Bridge v2)
  * @brief   STM32 串口桥接程序 - 用于给 ESP8266 烧录 MQTT AT 固件
  *          USART1(CH340/USB) <--透明转发--> USART3(ESP8266)
  *
  * ⚠️ 必须使用 esptool.py --no-stub 烧录，不能用 Flash Download Tool！
  *    原因：Flash Download Tool 会上传 stub 并切换波特率，桥接程序无法跟随。
  *    --no-stub 模式全程保持 115200，桥接程序可以稳定工作。
  *
  * v2 改进：
  *   1. 中断使能前清除所有挂起的 USART 标志，防止上电误触发
  *   2. NVIC 优先级分组显式设置，防止 ESP8266_Init 内部覆盖
  *   3. ESP8266 复位序列更完整（先 EN 低再 EN 高再 RST 脉冲）
  *   4. 主循环加入 LED2 心跳闪烁，确认桥接程序未死机
  *   5. 中断文件修复 IDLE 清除顺序 + ORE 溢出处理 + TXE 超时
  *
  * 使用方法：
  *   1. Keil 排除 main.c 和 stm32f1xx_it.c，包含本文件和 it_bridge.c
  *   2. 编译下载到 STM32（ST-Link 或 FlyMcu）
  *   3. ESP8266 跳帽：IO0 接 GND，EN 接 3V3（用跳线/拨钮）
  *   4. 按开发板 RESET 键，ESP8266 进入下载模式
  *   5. 关闭所有串口助手，用 esptool.py --no-stub 烧录：
  *      python -m esptool --chip esp8266 --port COM4 --baud 115200 --no-stub \
  *        write_flash --flash_mode dio --flash_freq 40m --flash_size 1MB \
  *        0x00000 "D:\modulebusLinkMQTT\Doc\ESP8266_AT_MQTT_1M.bin"
  *   6. 烧录成功后：IO0 跳帽改回 3V3，Keil 恢复 main.c + it.c
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f1xx.h"
#include "./usart/bsp_debug_usart.h"
#include "./ESP8266/bsp_esp8266.h"
#include "./led/bsp_led.h"
#include <stdio.h>

/* 外部变量：USART1 和 USART3 的 HAL 句柄 */
extern UART_HandleTypeDef UartHandle;   /* USART1 - CH340/USB */
extern UART_HandleTypeDef Uart3Handle;  /* USART3 - ESP8266 */

/* 内部函数声明 */
static void Bridge_ClearPendingFlags(void);

/**
  * @brief  清除两个串口上所有挂起的错误/状态标志
  *         上电时 SR 寄存器可能有残留标志，不清除会导致
  *         中断使能后立即误触发，干扰烧录通信。
  * @note   STM32F1xx 清除规则：读 SR 再读 DR 可清除 IDLE/ORE/NE/FE
  */
static void Bridge_ClearPendingFlags(void)
{
    /* USART1 - 读 SR 再读 DR，清除 IDLE/ORE/NE/FE */
    (void)UartHandle.Instance->SR;
    (void)UartHandle.Instance->DR;

    /* USART3 - 同上 */
    (void)Uart3Handle.Instance->SR;
    (void)Uart3Handle.Instance->DR;
}

/**
  * @brief  主函数 - 串口桥接模式
  * @param  无
  * @retval 无
  */
int main(void)
{
    uint32_t lastTick = 0;

    /* 配置系统时钟为 72 MHz */
    SystemClock_Config();

    /* NVIC 优先级分组 = 2（2 位抢占 + 2 位子优先级）
       必须在 ESP8266_Init() 之前设置，否则 ESP8266_Init 内部会覆盖 */
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_2);

    /* 初始化 LED（桥接模式指示灯） */
    LED_GPIO_Config();

    /* 初始化 USART1 (CH340/USB) 115200 8-N-1 */
    DEBUG_USART_Config();

    /* 初始化 USART3 (ESP8266) 115200 8-N-1 + GPIO(CH_PD=PG13, RST=PG14)
       注意：ESP8266_Init 内部会：
       - 配置 USART3 为 115200 8-N-1
       - 使能 USART3 的 RXNE + IDLE 中断
       - 使能 USART3 NVIC（优先级 1,0）
       - RST 拉高，CH_PD 拉低（禁用） */
    ESP8266_Init();

    /* ★ 清除上电残留标志（必须在使能 USART1 中断之前） */
    Bridge_ClearPendingFlags();

    /* 使能 USART1 接收中断（原工程中此中断被注释掉了，桥接模式需要打开） */
    __HAL_UART_ENABLE_IT(&UartHandle, USART_IT_RXNE);
    __HAL_UART_ENABLE_IT(&UartHandle, USART_IT_IDLE);
    HAL_NVIC_SetPriority(DEBUG_USART_IRQ, 0, 0);  /* USART1 优先级最高 */
    HAL_NVIC_EnableIRQ(DEBUG_USART_IRQ);

    /* ====== ESP8266 启动序列 ====== */

    /* 1. 先确保 EN(CH_PD) = 低，让 ESP8266 完全断电 */
    macESP8266_CH_DISABLE();
    HAL_Delay(50);

    /* 2. RST = 低（保持复位状态） */
    macESP8266_RST_LOW_LEVEL();
    HAL_Delay(10);

    /* 3. EN = 高（上电使能），此时 GPIO0 已被跳线拉低到 GND */
    macESP8266_CH_ENABLE();
    HAL_Delay(50);

    /* 4. RST 拉低→延时→拉高（产生复位脉冲） */
    macESP8266_RST_LOW_LEVEL();
    HAL_Delay(100);
    macESP8266_RST_HIGH_LEVEL();

    /* 5. 等待 ESP8266 Bootloader 启动 */
    HAL_Delay(500);

    /* ====== 桥接就绪指示 ====== */
    LED1_ON;    /* LED1 常亮 = 桥接模式已启动 */
    LED2_OFF;
    LED3_OFF;

    /*
     * 主循环：LED2 心跳闪烁 + 空闲等待
     * 所有数据转发在中断中完成：
     *   USART1 收到字节 -> 立即转发到 USART3（PC -> ESP8266）
     *   USART3 收到字节 -> 立即转发到 USART1（ESP8266 -> PC）
     */
    while (1)
    {
        /* LED2 每 500ms 翻转一次 = 桥接程序存活心跳 */
        if (HAL_GetTick() - lastTick >= 500)
        {
            lastTick = HAL_GetTick();
            LED2_TOGGLE;
        }
    }
}

/**
  * @brief  System Clock Configuration
  *         与原工程完��相同：HSE 8MHz × PLL9 = 72MHz
  */
void SystemClock_Config(void)
{
  RCC_ClkInitTypeDef clkinitstruct = {0};
  RCC_OscInitTypeDef oscinitstruct = {0};

  oscinitstruct.OscillatorType  = RCC_OSCILLATORTYPE_HSE;
  oscinitstruct.HSEState        = RCC_HSE_ON;
  oscinitstruct.HSEPredivValue  = RCC_HSE_PREDIV_DIV1;
  oscinitstruct.PLL.PLLState    = RCC_PLL_ON;
  oscinitstruct.PLL.PLLSource   = RCC_PLLSOURCE_HSE;
  oscinitstruct.PLL.PLLMUL      = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&oscinitstruct)!= HAL_OK)
  {
    while(1);
  }

  clkinitstruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2);
  clkinitstruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clkinitstruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clkinitstruct.APB2CLKDivider = RCC_HCLK_DIV1;
  clkinitstruct.APB1CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&clkinitstruct, FLASH_LATENCY_2)!= HAL_OK)
  {
    while(1);
  }
}

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
