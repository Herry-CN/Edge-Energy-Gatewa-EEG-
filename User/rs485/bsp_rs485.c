/**
  ******************************************************************************
  * @file    bsp_rs485.c
  * @brief   物理层对齐 User485ok：PC2+PD11 切方向，RXNE 收字节，5ms 静默成帧。
  ******************************************************************************
  */
#include "./rs485/bsp_rs485.h"

UART_HandleTypeDef Uart2Handle;

static uint8_t           s_rx[RS485_RX_BUF_LEN];
static volatile uint16_t s_rx_len      = 0;
static volatile uint32_t s_last_rx_ms  = 0;

static void rs485_delay_loop(volatile uint32_t n)
{
    while (n--) {
        __NOP();
    }
}

static void rs485_de_tx(void)
{
#if RS485_DE_ENABLE
    rs485_delay_loop(1000u);
    HAL_GPIO_WritePin(RS485_DE_PORT, RS485_DE_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(RS485_DE_PORT_ALT, RS485_DE_PIN_ALT, GPIO_PIN_SET);
    rs485_delay_loop(1000u);
#endif
}

static void rs485_de_rx(void)
{
#if RS485_DE_ENABLE
    rs485_delay_loop(1000u);
    HAL_GPIO_WritePin(RS485_DE_PORT, RS485_DE_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(RS485_DE_PORT_ALT, RS485_DE_PIN_ALT, GPIO_PIN_RESET);
    rs485_delay_loop(1000u);
#endif
}

static void rs485_putc(uint8_t ch)
{
    uint32_t guard = 0;

    RS485_USART->DR = (uint16_t)ch;
    while (((RS485_USART->SR & USART_SR_TXE) == 0) && (guard < 200000u)) {
        guard++;
    }
}

void RS485_Init(void)
{
    GPIO_InitTypeDef gpio;

    RS485_USART_CLK_ENABLE();
    RS485_GPIO_CLK_ENABLE();

    gpio.Pin   = RS485_TX_PIN;
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(RS485_TX_PORT, &gpio);

    gpio.Pin   = RS485_RX_PIN;
    gpio.Mode  = GPIO_MODE_AF_INPUT;
    gpio.Pull  = GPIO_NOPULL;
    HAL_GPIO_Init(RS485_RX_PORT, &gpio);

#if RS485_DE_ENABLE
    gpio.Pin   = RS485_DE_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(RS485_DE_PORT, &gpio);

    gpio.Pin = RS485_DE_PIN_ALT;
    HAL_GPIO_Init(RS485_DE_PORT_ALT, &gpio);
    rs485_de_rx();
#endif

    Uart2Handle.Instance          = RS485_USART;
    Uart2Handle.Init.BaudRate     = RS485_BAUDRATE;
    Uart2Handle.Init.WordLength   = UART_WORDLENGTH_8B;
    Uart2Handle.Init.StopBits     = UART_STOPBITS_1;
    Uart2Handle.Init.Parity       = UART_PARITY_NONE;
    Uart2Handle.Init.Mode         = UART_MODE_TX_RX;
    Uart2Handle.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    HAL_UART_Init(&Uart2Handle);

    __HAL_UART_ENABLE_IT(&Uart2Handle, UART_IT_RXNE);

    HAL_NVIC_SetPriority(RS485_USART_IRQn, 1, 1);
    HAL_NVIC_EnableIRQ(RS485_USART_IRQn);

    RS485_RxReset();
}

void RS485_RxReset(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    s_rx_len = 0;
    __set_PRIMASK(primask);
}

void RS485_Send(const uint8_t *data, uint16_t len)
{
    uint16_t i;
    uint32_t guard = 0;

    if (data == NULL || len == 0) return;

    RS485_RxReset();
    rs485_de_tx();

    for (i = 0; i < len; i++) {
        rs485_putc(data[i]);
    }

    while (((RS485_USART->SR & USART_SR_TC) == 0) && (guard < 200000u)) {
        guard++;
    }

    rs485_de_rx();
}

uint8_t RS485_RxReady(void)
{
    if (s_rx_len == 0u) return 0;
    if ((HAL_GetTick() - s_last_rx_ms) >= RS485_IFG_MS) return 1;
    return 0;
}

void RS485_RxAck(void)
{
    RS485_RxReset();
}

uint16_t RS485_RxLen(void)
{
    return s_rx_len;
}

const uint8_t *RS485_RxBuf(void)
{
    return s_rx;
}

void USART2_IRQHandler(void)
{
    uint32_t sr = RS485_USART->SR;

    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t ch = (uint8_t)(RS485_USART->DR & 0xFF);

        if (sr & USART_SR_RXNE) {
            s_last_rx_ms = HAL_GetTick();
            if (s_rx_len < (RS485_RX_BUF_LEN - 1u)) {
                s_rx[s_rx_len++] = ch;
            } else {
                s_rx_len = 0;
            }
        }
    }
}
