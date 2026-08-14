#ifndef __MODBUS_MASTER_H
#define __MODBUS_MASTER_H

/**
  ******************************************************************************
  * @file    modbus_master.h
  * @brief   Modbus‑RTU 主站（功能码 0x03 / 0x06）
  *
  * 接口对齐 Doc/STM32 软件架构设计 V1.md，读接口多一个输出缓冲。
  ******************************************************************************
  */

#include <stdint.h>

#define MB_OK             0
#define MB_ERR_PARAM     -1
#define MB_ERR_TIMEOUT   -2
#define MB_ERR_CRC       -3
#define MB_ERR_ADDR      -4
#define MB_ERR_FUNC      -5
#define MB_ERR_EXC       -6
#define MB_ERR_LEN       -7

#define MB_FC_READ_HOLD   0x03u
#define MB_FC_WRITE_SINGLE 0x06u

#define MB_MAX_REGS       64u
#define MB_TIMEOUT_MS     200u
#define MB_RETRY          0

int mb_read (uint8_t addr, uint16_t reg, uint16_t num, uint16_t *dst);
int mb_write(uint8_t addr, uint16_t reg, uint16_t value);

#endif /* __MODBUS_MASTER_H */
