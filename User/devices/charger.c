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
    CHG_REG_STATE, CHG_REG_ENABLE, CHG_REG_START_STOP, CHG_REG_MODE, CHG_REG_VOLTAGE,
    CHG_REG_TEMPERATURE,
    CHG_REG_ENERGY_CHARGE, CHG_REG_ENERGY_DISCHARGE,
};
static const uint16_t k_poll_num[] = { 2, 2, 1, 2, 4, 1, 1, 1 };
#define CHG_POLL_ITEMS  ((uint8_t)(sizeof(k_poll_reg) / sizeof(k_poll_reg[0])))

static uint16_t s_img[50];          /* 1002..1051 */
static uint8_t  s_fail     = 0;
static uint8_t  s_online   = 0;
static uint8_t  s_recover_ok = 0;
static uint32_t s_last_ms    = 0;
static uint32_t s_due_ms[CHG_POLL_ITEMS];
static uint8_t  s_rr_a = 0;
static uint8_t  s_rr_b = 0;
static uint8_t  s_rr_c = 0;

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

static int poll_index_of(uint16_t reg)
{
    int i;

    for (i = 0; i < (int)CHG_POLL_ITEMS; i++) {
        if (k_poll_reg[i] == reg) return i;
    }
    return -1;
}

static uint32_t poll_period_ms(uint8_t idx)
{
    uint16_t reg = k_poll_reg[idx];

    if (reg == CHG_REG_STATE || reg == CHG_REG_ENABLE ||
        reg == CHG_REG_START_STOP || reg == CHG_REG_MODE) {
        return CHG_POLL_MS;                                                   /* A 类 */
    }
    if (reg == CHG_REG_VOLTAGE || reg == CHG_REG_TEMPERATURE) {
        return (g_onoff_state == ONOFF_CHARGING) ? 1000u : 3000u;           /* B 类 */
    }
    return 5000u;                                                            /* C 类 */
}

static void poll_schedule_idx(uint8_t idx, uint32_t now, bool immediate)
{
    if (idx >= CHG_POLL_ITEMS) return;
    s_due_ms[idx] = immediate ? now : (now + poll_period_ms(idx));
}

static void poll_schedule_reg(uint16_t reg, uint32_t now)
{
    int idx = poll_index_of(reg);

    if (idx >= 0) poll_schedule_idx((uint8_t)idx, now, true);
}

static int poll_pick_group_due(const uint8_t *list, uint8_t count, uint8_t *rr, uint32_t now)
{
    uint8_t i;

    for (i = 0; i < count; i++) {
        uint8_t pos = (uint8_t)((*rr + i) % count);
        uint8_t idx = list[pos];
        if ((int32_t)(now - s_due_ms[idx]) >= 0) return (int)idx;
    }
    return -1;
}

static int poll_pick_due(uint32_t now)
{
    static const uint8_t k_group_a[] = { 0u, 1u, 2u, 3u };
    static const uint8_t k_group_b[] = { 4u, 5u };
    static const uint8_t k_group_c[] = { 6u, 7u };
    int idx;

    idx = poll_pick_group_due(k_group_a, (uint8_t)(sizeof(k_group_a) / sizeof(k_group_a[0])), &s_rr_a, now);
    if (idx >= 0) {
        s_rr_a = (uint8_t)((s_rr_a + 1u) % (sizeof(k_group_a) / sizeof(k_group_a[0])));
        return idx;
    }

    idx = poll_pick_group_due(k_group_b, (uint8_t)(sizeof(k_group_b) / sizeof(k_group_b[0])), &s_rr_b, now);
    if (idx >= 0) {
        s_rr_b = (uint8_t)((s_rr_b + 1u) % (sizeof(k_group_b) / sizeof(k_group_b[0])));
        return idx;
    }

    idx = poll_pick_group_due(k_group_c, (uint8_t)(sizeof(k_group_c) / sizeof(k_group_c[0])), &s_rr_c, now);
    if (idx >= 0) {
        s_rr_c = (uint8_t)((s_rr_c + 1u) % (sizeof(k_group_c) / sizeof(k_group_c[0])));
        return idx;
    }

    return -1;
}

static void charger_export_cache(void)
{
    uint16_t state      = img_get(CHG_REG_STATE);
    uint16_t start_stat = img_get(CHG_REG_START_STOP);
    uint16_t soc        = img_get(CHG_REG_SOC);
    int16_t  temp       = (int16_t)img_get(CHG_REG_TEMPERATURE);

    g_state_code           = state;
    g_fault_code           = img_get(CHG_REG_FAULT);
    g_enable               = img_get(CHG_REG_ENABLE);
    g_charge_power_setpoint = img_get(CHG_REG_CHARGE_POWER);
    g_current_voltage      = img_get(CHG_REG_VOLTAGE);
    g_current_current      = img_get(CHG_REG_CURRENT);
    g_current_power        = img_get(CHG_REG_POWER);
    g_soc                  = (soc > 100u) ? 100u : (uint8_t)soc;
    g_energy_charge        = (uint32_t)img_get(CHG_REG_ENERGY_CHARGE);
    g_energy_discharge     = (uint32_t)img_get(CHG_REG_ENERGY_DISCHARGE);
    g_temperature          = (int16_t)(temp - 40);
    g_mode_code            = img_get(CHG_REG_MODE);
    g_start_stop           = start_stat;
    g_start_stop_control   = img_get(CHG_REG_START_STOP_CTRL);

    if (!s_online) {
        g_onoff_state = ONOFF_OFFLINE;
        LED2_OFF;
    } else if (start_stat == CHG_STATUS_STARTED || g_current_current > 0u || g_current_power > 0u) {
        g_onoff_state = ONOFF_CHARGING;
        LED2_ON;
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

static void charger_prime_cache(void)
{
    uint8_t idx;

    for (idx = 1u; idx < CHG_POLL_ITEMS; idx++) {
        if (read_block(k_poll_reg[idx], k_poll_num[idx]) == MB_OK) {
            charger_export_cache();
        }
    }
}

static void charger_note_result(int rc, const char *what)
{
    if (rc == MB_OK) {
        s_fail   = 0;
        if (!s_online) {
            if (s_recover_ok < 255u) s_recover_ok++;
            if (s_recover_ok >= CHG_RECOVER_ONLINE) {
                s_online = 1;
            }
        } else {
            s_recover_ok = 0;
        }
        g_onoff_state = ONOFF_IDLE;
        return;
    }
    s_recover_ok = 0;
    if (s_fail < 255u) s_fail++;
    if (s_fail >= CHG_FAIL_OFFLINE && s_online) {
        s_online = 0;
        g_onoff_state = ONOFF_OFFLINE;
        LED2_OFF;
        printf("[CHG] offline after %u %s failures\r\n",
               (unsigned)s_fail, what);
    }
}

void charger_init(void)
{
    int rc;
    uint8_t i;

    RS485_Init();
    s_fail       = 0;
    s_online     = 0;
    s_recover_ok = 0;
    s_last_ms    = 0;
    s_rr_a       = 0;
    s_rr_b       = 0;
    s_rr_c       = 0;
    for (i = 0; i < CHG_POLL_ITEMS; i++) s_due_ms[i] = 0;

    printf("\r\n[CHG] Modbus-RTU master  USART2 %lu 8N1  slave=%u\r\n"
           "      DE=PC2+PD11 TX=PA2 RX=PA3  (same as User485ok)\r\n",
           (unsigned long)RS485_BAUDRATE, (unsigned)CHG_SLAVE_ADDR);

    rc = charger_probe();
    charger_note_result(rc, "probe");
    if (rc == MB_OK) {
        s_online     = 1;
        s_recover_ok = 0;
        charger_prime_cache();
        charger_export_cache();
        printf("[CHG] probe OK  state=%u fault=%u U=%u I=%u P=%u\r\n",
               (unsigned)img_get(CHG_REG_STATE),
               (unsigned)img_get(CHG_REG_FAULT),
               (unsigned)img_get(CHG_REG_VOLTAGE),
               (unsigned)img_get(CHG_REG_CURRENT),
               (unsigned)img_get(CHG_REG_POWER));
    } else {
        s_due_ms[0] = HAL_GetTick() + CHG_OFFLINE_PROBE_MS;
        printf("[CHG] probe FAIL rc=%d  - check COM7 Modbus Server, 9600 8N1, ID=%u\r\n",
               rc, (unsigned)CHG_SLAVE_ADDR);
    }
}

void charger_poll(void)
{
    uint32_t now = HAL_GetTick();
    int      rc;
    int      idx;

    if (s_last_ms != 0u && (now - s_last_ms) < CHG_POLL_MS) return;
    s_last_ms = now;

    if (!s_online) {
        if (s_due_ms[0] != 0u && (int32_t)(now - s_due_ms[0]) < 0) return;
        idx = 0;   /* offline 只探测 state/fault */
    } else {
        idx = poll_pick_due(now);
        if (idx < 0) return;
    }

    /* 每次主循环只发一帧，避免堵死 MQTT AT */
    rc = read_block(k_poll_reg[idx], k_poll_num[idx]);
    charger_note_result(rc, "poll");
    if (rc == MB_OK) {
        charger_export_cache();
        poll_schedule_idx((uint8_t)idx, now, false);
        if (k_poll_reg[idx] == CHG_REG_VOLTAGE) {
            printf("[CHG] poll  state=%u U=%u I=%u P=%u soc=%u T=%d\r\n",
                   (unsigned)img_get(CHG_REG_STATE),
                   (unsigned)img_get(CHG_REG_VOLTAGE),
                   (unsigned)img_get(CHG_REG_CURRENT),
                   (unsigned)img_get(CHG_REG_POWER),
                   (unsigned)img_get(CHG_REG_SOC),
                   (int)((int16_t)img_get(CHG_REG_TEMPERATURE) - 40));
        }
    } else if (!s_online) {
        s_due_ms[0] = now + CHG_OFFLINE_PROBE_MS;
    } else {
        poll_schedule_idx((uint8_t)idx, now, false);
    }
}

bool charger_is_online(void)
{
    return s_online ? true : false;
}

bool charger_cmd_start(void)
{
    uint32_t now = HAL_GetTick();
    int rc = mb_write(CHG_SLAVE_ADDR, PDU(CHG_REG_START_STOP_CTRL), CHG_CTRL_START_VALUE);
    charger_note_result(rc, "start");
    if (rc != MB_OK) return false;
    img_set(CHG_REG_START_STOP_CTRL, CHG_CTRL_START_VALUE);
    charger_export_cache();
    poll_schedule_reg(CHG_REG_START_STOP, now);
    poll_schedule_reg(CHG_REG_MODE, now);
    return true;
}

bool charger_cmd_stop(void)
{
    uint32_t now = HAL_GetTick();
    int rc = mb_write(CHG_SLAVE_ADDR, PDU(CHG_REG_START_STOP_CTRL), CHG_CTRL_STOP_VALUE);
    charger_note_result(rc, "stop");
    if (rc != MB_OK) return false;
    img_set(CHG_REG_START_STOP_CTRL, CHG_CTRL_STOP_VALUE);
    img_set(CHG_REG_CURRENT, 0u);
    img_set(CHG_REG_POWER, 0u);
    charger_export_cache();
    poll_schedule_reg(CHG_REG_START_STOP, now);
    poll_schedule_reg(CHG_REG_MODE, now);
    return true;
}

bool charger_cmd_set_power(uint16_t power_x10)
{
    uint32_t now = HAL_GetTick();
    int rc = mb_write(CHG_SLAVE_ADDR, PDU(CHG_REG_CHARGE_POWER), power_x10);
    charger_note_result(rc, "set_power");
    if (rc != MB_OK) return false;
    img_set(CHG_REG_CHARGE_POWER, power_x10);
    g_charge_power_setpoint = power_x10;
    poll_schedule_reg(CHG_REG_ENABLE, now);
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
