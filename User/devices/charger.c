/**
  ******************************************************************************
  * @file    charger.c
  * @brief   充电桩：轮询保持寄存器，刷新 EEG 已有 g_* 缓存；执行启停/功率写。
  ******************************************************************************
  */
#include "./devices/charger.h"
#include "./modbus/modbus_master.h"
#include "./rs485/bsp_rs485.h"
#include "./ESP8266/bsp_esp8266_test.h"
#include "./ESP8266/bsp_eeg_proto.h"
#include "./ESP8266/bsp_esp8266_mqtt.h"
#include "./led/bsp_led.h"
#include "stm32f1xx.h"
#include <stdio.h>

#if MB_MASTER_ENABLE

#define PDU(reg)   ((uint16_t)((int)(reg) + (int)(CHG_REG_OFFSET)))

static const uint16_t k_poll_reg[] = {
    CHG_REG_STATE, CHG_REG_ENABLE, CHG_REG_VOLTAGE,
    CHG_REG_ENERGY_CHARGE, CHG_REG_ENERGY_DISCHARGE,
    CHG_REG_TEMPERATURE, CHG_REG_MODE
};
static const uint16_t k_poll_num[] = { 2, 1, 4, 1, 1, 1, 2 };
#define CHG_POLL_ITEMS  ((uint8_t)(sizeof(k_poll_reg) / sizeof(k_poll_reg[0])))

static uint16_t s_img[50];          /* 1001..1050 */
static uint8_t  s_fail     = 0;
static uint8_t  s_online   = 0;
static uint32_t s_last_ms  = 0;
static uint8_t  s_poll_idx = 0;

static uint16_t img_get(uint16_t reg)
{
    uint16_t idx = (uint16_t)(reg - CHG_REG_STATE);
    if (idx >= 50u) return 0;
    return s_img[idx];
}

static void img_set(uint16_t reg, uint16_t val)
{
    uint16_t idx = (uint16_t)(reg - CHG_REG_STATE);
    if (idx < 50u) s_img[idx] = val;
}

static void charger_export_cache(void)
{
    uint16_t state = img_get(CHG_REG_STATE);
    uint16_t start = img_get(CHG_REG_START_STOP);
    uint16_t soc   = img_get(CHG_REG_SOC);
    uint32_t echg  = (uint32_t)img_get(CHG_REG_ENERGY_CHARGE)
                   | ((uint32_t)img_get((uint16_t)(CHG_REG_ENERGY_CHARGE + 1u)) << 16);

    g_fault_code      = img_get(CHG_REG_FAULT);
    g_current_voltage = img_get(CHG_REG_VOLTAGE);
    g_current_current = img_get(CHG_REG_CURRENT);
    g_current_power   = img_get(CHG_REG_POWER);
    g_soc             = (soc > 100u) ? 100u : (uint8_t)soc;
    g_energy_charge   = echg;
    g_temperature     = (int16_t)img_get(CHG_REG_TEMPERATURE);

    if (state == CHG_STATE_CHARGING || start == CHG_START_VALUE) {
        g_onoff_state = ONOFF_CHARGING;
        LED2_ON;
    } else if (state == CHG_STATE_OFFLINE) {
        g_onoff_state = ONOFF_OFFLINE;
        LED2_OFF;
    } else {
        g_onoff_state = ONOFF_IDLE;
        LED2_OFF;
    }
}

static int read_block(uint16_t reg, uint16_t num)
{
    uint16_t tmp[MB_MAX_REGS];
    uint16_t i;
    int      rc;

    rc = mb_read(CHG_SLAVE_ADDR, PDU(reg), num, tmp);
    if (rc != MB_OK) return rc;
    for (i = 0; i < num; i++) {
        img_set((uint16_t)(reg + i), tmp[i]);
    }
    return MB_OK;
}

static int charger_probe(void)
{
    int rc = read_block(CHG_REG_STATE, 2);
    if (rc != MB_OK) return rc;
    charger_export_cache();
    return MB_OK;
}

static void charger_note_result(int rc, const char *what)
{
    if (rc == MB_OK) {
        s_fail   = 0;
        s_online = 1;
        return;
    }
    if (s_fail < 255u) s_fail++;
    if (s_fail >= CHG_FAIL_OFFLINE && s_online) {
        s_online = 0;
        printf("[CHG] offline after %u %s failures\r\n",
               (unsigned)s_fail, what);
    }
}

void charger_init(void)
{
    int rc;

    RS485_Init();
    s_fail    = 0;
    s_online  = 0;
    s_last_ms = 0;

    printf("\r\n[CHG] Modbus-RTU master  USART2 %lu 8N1  slave=%u\r\n"
           "      DE=PC2+PD11 TX=PA2 RX=PA3  (same as User485ok)\r\n",
           (unsigned long)RS485_BAUDRATE, (unsigned)CHG_SLAVE_ADDR);

    rc = charger_probe();
    charger_note_result(rc, "probe");
    if (rc == MB_OK) {
        printf("[CHG] probe OK  state=%u fault=%u U=%u I=%u P=%u\r\n",
               (unsigned)img_get(CHG_REG_STATE),
               (unsigned)img_get(CHG_REG_FAULT),
               (unsigned)img_get(CHG_REG_VOLTAGE),
               (unsigned)img_get(CHG_REG_CURRENT),
               (unsigned)img_get(CHG_REG_POWER));
    } else {
        printf("[CHG] probe FAIL rc=%d  - check COM7 Modbus Server, 9600 8N1, ID=%u\r\n",
               rc, (unsigned)CHG_SLAVE_ADDR);
    }
}

void charger_poll(void)
{
    uint32_t now = HAL_GetTick();
    int      rc;

    if (s_last_ms != 0u && (now - s_last_ms) < CHG_POLL_MS) return;
    s_last_ms = now;

    /* 与 User485ok 一样：每次主循环只发一帧，避免堵死 MQTT AT */
    rc = read_block(k_poll_reg[s_poll_idx], k_poll_num[s_poll_idx]);
    charger_note_result(rc, "poll");
    if (rc == MB_OK) {
        charger_export_cache();
        if (k_poll_reg[s_poll_idx] == CHG_REG_VOLTAGE) {
            printf("[CHG] poll  state=%u U=%u I=%u P=%u soc=%u T=%d\r\n",
                   (unsigned)img_get(CHG_REG_STATE),
                   (unsigned)img_get(CHG_REG_VOLTAGE),
                   (unsigned)img_get(CHG_REG_CURRENT),
                   (unsigned)img_get(CHG_REG_POWER),
                   (unsigned)img_get(CHG_REG_SOC),
                   (int)(int16_t)img_get(CHG_REG_TEMPERATURE));
        }
    }
    s_poll_idx++;
    if (s_poll_idx >= CHG_POLL_ITEMS) s_poll_idx = 0;
}

bool charger_is_online(void)
{
    return s_online ? true : false;
}

bool charger_cmd_start(void)
{
    int rc = mb_write(CHG_SLAVE_ADDR, PDU(CHG_REG_START_STOP), CHG_START_VALUE);
    charger_note_result(rc, "start");
    if (rc != MB_OK) return false;
    img_set(CHG_REG_START_STOP, CHG_START_VALUE);
    img_set(CHG_REG_STATE, CHG_STATE_CHARGING);
    charger_export_cache();
    return true;
}

bool charger_cmd_stop(void)
{
    int rc = mb_write(CHG_SLAVE_ADDR, PDU(CHG_REG_START_STOP), CHG_STOP_VALUE);
    charger_note_result(rc, "stop");
    if (rc != MB_OK) return false;
    img_set(CHG_REG_START_STOP, CHG_STOP_VALUE);
    img_set(CHG_REG_STATE, CHG_STATE_IDLE);
    charger_export_cache();
    return true;
}

bool charger_cmd_set_power(uint16_t power_x10)
{
    int rc = mb_write(CHG_SLAVE_ADDR, PDU(CHG_REG_CHARGE_POWER), power_x10);
    charger_note_result(rc, "set_power");
    if (rc != MB_OK) return false;
    img_set(CHG_REG_CHARGE_POWER, power_x10);
    return true;
}

#else /* !MB_MASTER_ENABLE */

void charger_init(void) {}
void charger_poll(void) {}
bool charger_is_online(void) { return false; }
bool charger_cmd_start(void) { return false; }
bool charger_cmd_stop(void) { return false; }
bool charger_cmd_set_power(uint16_t power_x10) { (void)power_x10; return false; }

#endif
