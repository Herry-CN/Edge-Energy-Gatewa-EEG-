#ifndef __MODBUS_CRC_H
#define __MODBUS_CRC_H

#include <stdint.h>

uint16_t mb_crc16(const uint8_t *buf, uint16_t len);

#endif /* __MODBUS_CRC_H */
