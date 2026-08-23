/**
  ******************************************************************************
  * @file    charger.c
  * @brief   按点表分组读保持寄存器。禁止跨空地址 1012-1020 / 1036 / 1038 / 1043-1047。
  *          每时隙一帧。G2 拆成两段，避免从站一次 15 个寄存器拒答导致 1028 起全是 0。
  ******************************************************************************
  */
#include "./devices/charger.h"
#include "./modbus/modbus_master.h"
#include "./rs485/bsp_rs485.h"
#include "./ESP8266/bsp_esp8266_test.h"
#include "./ESP8266/bsp_eeg_proto.h"
#include "./ESP8266/bsp_esp8266_mqtt.h"
#include "./config/bsp_config.h"
#include "./led/bsp_led.h"
#include "stm32f1xx.h"
#include <stdio.h>

#if MB_MASTER_ENABLE

#define PDU(reg)   ((uint16_t)((int)(reg) + (int)(CHG_REG_OFFSET)))

/*
 * MQTT 用到的点必须能单独读成功。mbserver 对「一次 10/15 个」常只填前几个，
 * 后半段（1028 电压、1031 SOC、1035 电表、1039 温度）会变成 0。
 * 禁止跨空地址 1012-1020 / 1036 / 1038 / 1043-1047。
 */
static const uint16_t k_poll_reg[] = {
    1001,  /* 1001-1011 桩状态、进线 */
    1021,  /* 1021-1025 设定/使能 */
    1028,  /* 输出电压 → MQTT voltage */
    1029,  /* 输出电流 → MQTT current */
    1030,  /* 输出功率 → MQTT power */
    1031,  /* SOC */
    1034,  /* 启停状态 */
    1035,  /* 充电电表 */
    1037,  /* 放电电表 */
    1039,  /* 枪温 */
    1048   /* 1048-1050 目标SOC/模式/启停控制 */
};
static const uint16_t k_poll_num[] = { 11, 5, 1, 1, 1, 1, 1, 1, 1, 1, 3 };
#define CHG_POLL_ITEMS  ((uint8_t)(sizeof(k_poll_reg) / sizeof(k_poll_reg[0])))

static const uint16_t k_rw_list[] = {
    1021, 1022, 1023, 1024, 1025, 1048, 1049, 1050
};

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

/* 该组读失败则清零，禁止把上一轮测试值一直发到 MQTT */
static void img_clear_range(uint16_t reg, uint16_t num)
{
    uint16_t i;
    for (i = 0; i < num; i++) {
        img_set((uint16_t)(reg + i), 0);
    }
}

static int is_rw(uint16_t n)
{
    uint8_t i;
    for (i = 0; i < (uint8_t)(sizeof(k_rw_list) / sizeof(k_rw_list[0])); i++) {
        if (k_rw_list[i] == n) return 1;
    }
    return 0;
}

static void charger_export_cache(void)
{
    uint16_t pile = img_get(CHG_REG_STATE);
    uint16_t ss   = img_get(CHG_REG_START_STOP_STATE);
    uint16_t ctrl = img_get(CHG_REG_START_STOP);
    uint16_t soc  = img_get(CHG_REG_SOC);
    uint16_t raw_t = img_get(CHG_REG_TEMPERATURE);

    g_charger_state      = pile;
    g_fault_code         = img_get(CHG_REG_FAULT);
    g_capability_word    = img_get(CHG_REG_CAPABILITY);
    g_module_count       = img_get(CHG_REG_MODULE_COUNT);
    g_enable_word        = img_get(CHG_REG_ENABLE);
    g_start_stop_state   = ss;
    g_start_stop_control = ctrl;
    g_work_mode          = img_get(CHG_REG_MODE);
    g_charge_power_x10   = img_get(CHG_REG_CHARGE_POWER);

    g_input_voltage      = img_get(CHG_REG_INPUT_VOLTAGE);
    g_input_current      = img_get(CHG_REG_INPUT_CURRENT);
    g_input_power        = img_get(CHG_REG_INPUT_POWER);

    /* MQTT voltage/current/power 必须是枪输出 1028/1029/1030，不是进线 1009/1010/1011 */
    g_current_voltage    = img_get(CHG_REG_VOLTAGE);
    g_current_current    = img_get(CHG_REG_CURRENT);
    g_current_power      = img_get(CHG_REG_POWER);

    g_soc                = (soc > 100u) ? 100u : (uint8_t)soc;
    g_energy_charge      = img_get(CHG_REG_ENERGY_CHARGE);
    g_energy_discharge   = img_get(CHG_REG_ENERGY_DISCHARGE);
    g_temperature        = (int16_t)raw_t - (int16_t)CHG_TEMP_OFFSET;

    if (ctrl == CHG_START_VALUE) {
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

static void charger_dump(const char *tag)
{
    if (!Config_LogVerbose()) return;
    printf("[CHG] %s  1001=%u 1003=%u 1004=%u 1009=%u 1010=%u 1023=%u\r\n"
           "      1028U=%u 1029I=%u 1030P=%u  1031soc=%u 1035Ec=%u 1037Ed=%u 1039T=%u\r\n",
           tag,
           (unsigned)img_get(CHG_REG_STATE),
           (unsigned)img_get(CHG_REG_CAPABILITY),
           (unsigned)img_get(CHG_REG_MODULE_COUNT),
           (unsigned)img_get(CHG_REG_INPUT_VOLTAGE),
           (unsigned)img_get(CHG_REG_INPUT_CURRENT),
           (unsigned)img_get(CHG_REG_ENABLE),
           (unsigned)img_get(CHG_REG_VOLTAGE),
           (unsigned)img_get(CHG_REG_CURRENT),
           (unsigned)img_get(CHG_REG_POWER),
           (unsigned)img_get(CHG_REG_SOC),
           (unsigned)img_get(CHG_REG_ENERGY_CHARGE),
           (unsigned)img_get(CHG_REG_ENERGY_DISCHARGE),
           (unsigned)img_get(CHG_REG_TEMPERATURE));
}

void charger_init(void)
{
    uint8_t i;
    int     rc;
    int     any_ok = 0;

    RS485_Init();
    s_fail    = 0;
    s_online  = 0;
    s_last_ms = 0;

    printf("\r\n[CHG] Modbus-RTU master  USART2 %lu 8N1  slave=%u\r\n"
           "      PDU=N  mbserver 401029 = 点表 1028 输出电压\r\n",
           (unsigned long)RS485_BAUDRATE, (unsigned)CHG_SLAVE_ADDR);

    for (i = 0; i < CHG_POLL_ITEMS; i++) {
        rc = read_block(k_poll_reg[i], k_poll_num[i]);
        charger_note_result(rc, "probe");
        if (rc == MB_OK) any_ok = 1;
        else {
            printf("[CHG] probe FAIL group %u@%u n=%u rc=%d\r\n",
                   (unsigned)i, (unsigned)k_poll_reg[i],
                   (unsigned)k_poll_num[i], rc);
        }
    }
    if (any_ok) {
        charger_export_cache();
        charger_dump("probe OK");
    } else {
        printf("[CHG] probe FAIL all groups  - check 9600 8N1 ID=%u\r\n",
               (unsigned)CHG_SLAVE_ADDR);
    }
}

void charger_poll(void)
{
    uint32_t now = HAL_GetTick();
    int      rc;

    if (s_last_ms != 0u && (now - s_last_ms) < CHG_POLL_MS) return;
    s_last_ms = now;

    rc = read_block(k_poll_reg[s_poll_idx], k_poll_num[s_poll_idx]);
    charger_note_result(rc, "poll");
    if (rc == MB_OK) {
        charger_export_cache();
        if (k_poll_num[s_poll_idx] == 1u ||
            k_poll_reg[s_poll_idx] == 1001u ||
            k_poll_reg[s_poll_idx] == 1028u) {
            charger_dump("poll");
        }
    } else {
        /* 只清本帧，禁止把 1023 使能和 1028 电压绑在一次失败里一起清掉 */
        img_clear_range(k_poll_reg[s_poll_idx], k_poll_num[s_poll_idx]);
        charger_export_cache();
        printf("[CHG] poll FAIL @%u n=%u rc=%d  this point cleared\r\n",
               (unsigned)k_poll_reg[s_poll_idx],
               (unsigned)k_poll_num[s_poll_idx], rc);
    }
    s_poll_idx++;
    if (s_poll_idx >= CHG_POLL_ITEMS) s_poll_idx = 0;
}

bool charger_is_online(void)
{
    return s_online ? true : false;
}

uint16_t charger_reg(uint16_t n)
{
    return img_get(n);
}

bool charger_cmd_write(uint16_t n, uint16_t value)
{
    int rc;

    if (!is_rw(n)) {
        printf("[CHG] write denied N=%u (R only)\r\n", (unsigned)n);
        return false;
    }
    rc = mb_write(CHG_SLAVE_ADDR, PDU(n), value);
    charger_note_result(rc, "write");
    if (rc != MB_OK) return false;
    img_set(n, value);
    charger_export_cache();
    return true;
}

bool charger_cmd_start(void)
{
    return charger_cmd_write(CHG_REG_START_STOP, CHG_START_VALUE);
}

bool charger_cmd_stop(void)
{
    return charger_cmd_write(CHG_REG_START_STOP, CHG_STOP_VALUE);
}

bool charger_cmd_set_power(uint16_t power_x10)
{
    return charger_cmd_write(CHG_REG_CHARGE_POWER, power_x10);
}

#else /* !MB_MASTER_ENABLE */

void     charger_init(void) {}
void     charger_poll(void) {}
bool     charger_is_online(void) { return false; }
bool     charger_cmd_start(void) { return false; }
bool     charger_cmd_stop(void) { return false; }
bool     charger_cmd_set_power(uint16_t power_x10) { (void)power_x10; return false; }
bool     charger_cmd_write(uint16_t n, uint16_t value) { (void)n; (void)value; return false; }
uint16_t charger_reg(uint16_t n) { (void)n; return 0; }

#endif
