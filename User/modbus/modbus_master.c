/**
  ******************************************************************************
  * @file    modbus_master.c
  * @brief   Modbus‑RTU 主站：组帧 / 等从站应答 / CRC 校验。
  ******************************************************************************
  */
#include "./modbus/modbus_master.h"
#include "./modbus/modbus_crc.h"
#include "./rs485/bsp_rs485.h"
#include "./wdg/bsp_iwdg.h"
#include "stm32f1xx.h"
#include <stdio.h>
#include <string.h>

#ifndef MB_TRACE
#define MB_TRACE  1
#endif

static void mb_dump(const char *tag, const uint8_t *p, uint16_t n)
{
#if MB_TRACE
    uint16_t i;
    printf("[MB %s]", tag);
    for (i = 0; i < n; i++) {
        printf(" %02X", p[i]);
    }
    printf("\r\n");
#else
    (void)tag; (void)p; (void)n;
#endif
}

static int mb_wait_rx(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms) {
        IWDG_Feed();
        if (RS485_RxReady()) return MB_OK;
    }
    return MB_ERR_TIMEOUT;
}

static int mb_check_crc(const uint8_t *p, uint16_t n)
{
    uint16_t crc, got;

    if (n < 4u) return MB_ERR_LEN;
    crc = mb_crc16(p, (uint16_t)(n - 2u));
    got = (uint16_t)p[n - 2u] | ((uint16_t)p[n - 1u] << 8);
    return (crc == got) ? MB_OK : MB_ERR_CRC;
}

static int mb_transact(const uint8_t *tx, uint16_t txlen,
                       uint8_t *rx, uint16_t rxmax, uint16_t *rxlen)
{
    int      attempt;
    int      rc;
    uint16_t n;

    for (attempt = 0; attempt <= MB_RETRY; attempt++) {
        mb_dump("TX", tx, txlen);
        RS485_Send(tx, txlen);

        rc = mb_wait_rx(MB_TIMEOUT_MS);
        if (rc != MB_OK) {
            printf("[MB] timeout (try %d)\r\n", attempt + 1);
            continue;
        }

        n = RS485_RxLen();
        if (n > rxmax) n = rxmax;
        memcpy(rx, RS485_RxBuf(), n);
        RS485_RxAck();
        mb_dump("RX", rx, n);

        /* 0x03 请求 8 字节、应答更长：收到与请求完全相同的 8 字节才是本机回声。
         * 0x06 的从站应答协议上就是请求的拷贝，绝不能当回声丢掉，否则 MQTT
         * start/stop 会 ACK 失败，而 Mbserver 其实已经写进去了。 */
        if (tx[1] == MB_FC_READ_HOLD && n == txlen && memcmp(rx, tx, n) == 0) {
            rc = mb_wait_rx(MB_TIMEOUT_MS);
            if (rc != MB_OK) {
                printf("[MB] echo only, no slave reply\r\n");
                continue;
            }
            n = RS485_RxLen();
            if (n > rxmax) n = rxmax;
            memcpy(rx, RS485_RxBuf(), n);
            RS485_RxAck();
            mb_dump("RX", rx, n);
        }

        rc = mb_check_crc(rx, n);
        if (rc != MB_OK) {
            printf("[MB] bad CRC\r\n");
            continue;
        }

        *rxlen = n;
        return MB_OK;
    }
    return MB_ERR_TIMEOUT;
}

int mb_read(uint8_t addr, uint16_t reg, uint16_t num, uint16_t *dst)
{
    uint8_t  tx[8];
    uint8_t  rx[5u + MB_MAX_REGS * 2u];
    uint16_t rxlen = 0;
    uint16_t crc;
    uint16_t i;
    int      rc;

    if (dst == NULL || num == 0u || num > MB_MAX_REGS) return MB_ERR_PARAM;

    tx[0] = addr;
    tx[1] = MB_FC_READ_HOLD;
    tx[2] = (uint8_t)(reg >> 8);
    tx[3] = (uint8_t)(reg & 0xFFu);
    tx[4] = (uint8_t)(num >> 8);
    tx[5] = (uint8_t)(num & 0xFFu);
    crc   = mb_crc16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFFu);
    tx[7] = (uint8_t)(crc >> 8);

    rc = mb_transact(tx, 8, rx, sizeof(rx), &rxlen);
    if (rc != MB_OK) return rc;

    if (rx[0] != addr) return MB_ERR_ADDR;
    if (rx[1] == (uint8_t)(MB_FC_READ_HOLD | 0x80u)) {
        printf("[MB] exception fc=03 code=%u\r\n", (unsigned)rx[2]);
        return MB_ERR_EXC;
    }
    if (rx[1] != MB_FC_READ_HOLD) return MB_ERR_FUNC;
    if (rxlen < 5u || rx[2] != (uint8_t)(num * 2u) || rxlen < (uint16_t)(5u + num * 2u)) {
        return MB_ERR_LEN;
    }

    for (i = 0; i < num; i++) {
        dst[i] = ((uint16_t)rx[3u + i * 2u] << 8) | rx[4u + i * 2u];
    }
    return MB_OK;
}

int mb_write(uint8_t addr, uint16_t reg, uint16_t value)
{
    uint8_t  tx[8];
    uint8_t  rx[8];
    uint16_t rxlen = 0;
    uint16_t crc;
    int      rc;

    tx[0] = addr;
    tx[1] = MB_FC_WRITE_SINGLE;
    tx[2] = (uint8_t)(reg >> 8);
    tx[3] = (uint8_t)(reg & 0xFFu);
    tx[4] = (uint8_t)(value >> 8);
    tx[5] = (uint8_t)(value & 0xFFu);
    crc   = mb_crc16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFFu);
    tx[7] = (uint8_t)(crc >> 8);

    rc = mb_transact(tx, 8, rx, sizeof(rx), &rxlen);
    if (rc != MB_OK) return rc;

    if (rx[0] != addr) return MB_ERR_ADDR;
    if (rx[1] == (uint8_t)(MB_FC_WRITE_SINGLE | 0x80u)) {
        printf("[MB] exception fc=06 code=%u\r\n", (unsigned)rx[2]);
        return MB_ERR_EXC;
    }
    if (rx[1] != MB_FC_WRITE_SINGLE || rxlen < 8u) return MB_ERR_FUNC;
    if (rx[2] != tx[2] || rx[3] != tx[3] || rx[4] != tx[4] || rx[5] != tx[5]) {
        return MB_ERR_LEN;
    }
    return MB_OK;
}
