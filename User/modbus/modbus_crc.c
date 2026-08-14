#include "./modbus/modbus_crc.h"
#include <stddef.h>

uint16_t mb_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i, b;

    if (buf == NULL) return 0;

    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (b = 0; b < 8u; b++) {
            if (crc & 1u) crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            else          crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}
