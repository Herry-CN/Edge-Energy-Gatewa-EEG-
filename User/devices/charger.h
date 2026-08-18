#ifndef __CHARGER_H
#define __CHARGER_H

/**
  ******************************************************************************
  * @file    charger.h
  * @brief   充电桩设备层：严格按 Doc/RS485与V2G充电桩通信点表.md
  *
  * 逻辑地址 N = 点表「寄存器地址」= RTU PDU。
  * mbserver 6 位显示 = 400001 + N，例如 401029 = 输出电压 1028。
  * R 只读状态；R/W 才允许写（控制）。
  ******************************************************************************
  */

#include <stdint.h>
#include <stdbool.h>

#define MB_MASTER_ENABLE                1

#define CHG_SLAVE_ADDR                  1u

/*
 * PDU 地址 = 点表逻辑地址 N。
 * 若 Modbus Server 勾了 “PLC 地址 / 1-based”（界面 1001 = 报文 1000），
 * 把 CHG_REG_OFFSET 改成 -1。
 */
#define CHG_REG_OFFSET                  0

/* —— 点表逻辑地址（与 401xxx 一一对应：401002=1001 … 401051=1050）—— */
#define CHG_REG_STATE                   1001    /* R    桩状态 0正常 1故障 2报警 */
#define CHG_REG_FAULT                   1002    /* R    故障码 */
#define CHG_REG_CAPABILITY              1003    /* R    是否支持放电、枪数量 */
#define CHG_REG_MODULE_COUNT            1004    /* R    充电模块数量 */
#define CHG_REG_RATED_CHARGE            1005    /* R    额定充电功率 0.1kW */
#define CHG_REG_RATED_DISCHARGE         1006    /* R    额定放电功率 0.1kW */
#define CHG_REG_BMS_CHARGE_DEMAND       1007    /* R    车BMS需求充电功率 0.1kW */
#define CHG_REG_PILE_OUTPUT_POWER       1008    /* R    桩端实际输出功率 0.1kW */
#define CHG_REG_INPUT_VOLTAGE           1009    /* R    进线电压 0.1V */
#define CHG_REG_INPUT_CURRENT           1010    /* R    进线电流 0.01A */
#define CHG_REG_INPUT_POWER             1011    /* R    进线功率 0.1kW */
#define CHG_REG_OFFLINE_MAX_DISCHARGE   1021    /* R/W  EMS离线最大放电 0.1kW */
#define CHG_REG_OFFLINE_MAX_CHARGE      1022    /* R/W  EMS离线最大充电 0.1kW */
#define CHG_REG_ENABLE                  1023    /* R/W  充/放电使能字 */
#define CHG_REG_CHARGE_POWER            1024    /* R/W  充电功率设定 0.1kW */
#define CHG_REG_DISCHARGE_POWER         1025    /* R/W  放电功率设定 0.1kW */
#define CHG_REG_GUN_ATTR                1026    /* R    枪属性、当前状态 */
#define CHG_REG_GUN_POWER_LIMIT         1027    /* R    枪固有功率限制 */
#define CHG_REG_VOLTAGE                 1028    /* R    输出电压 0.1V  → MQTT voltage */
#define CHG_REG_CURRENT                 1029    /* R    输出电流 0.01A → MQTT current */
#define CHG_REG_POWER                   1030    /* R    输出功率 0.1kW → MQTT power */
#define CHG_REG_SOC                     1031    /* R    车当前SOC % */
#define CHG_REG_BMS_VOLTAGE_DEMAND      1032    /* R    车BMS需求电压 0.1V */
#define CHG_REG_BMS_CURRENT_DEMAND      1033    /* R    车BMS需求电流 0.01A */
#define CHG_REG_START_STOP_STATE        1034    /* R    启停状态 0启机 1待机 2停机 */
#define CHG_REG_ENERGY_CHARGE           1035    /* R    充电电表 0.1kWh */
/* 1036 空 */
#define CHG_REG_ENERGY_DISCHARGE        1037    /* R    放电电表 0.01kWh */
/* 1038 空 */
#define CHG_REG_TEMPERATURE             1039    /* R    枪温 raw，工程量 = raw-40 */
#define CHG_REG_ETA_MIN                 1040    /* R    预计完成 min */
#define CHG_REG_CHARGE_DURATION         1041    /* R    本次时长 min */
#define CHG_REG_SESSION_ENERGY          1042    /* R    本次电量 0.01kWh */
#define CHG_REG_TARGET_SOC              1048    /* R/W  目标SOC 0.1% */
#define CHG_REG_MODE                    1049    /* R/W  0 CHG  1 V2G */
#define CHG_REG_START_STOP              1050    /* R/W  启停控制（写值以厂家为准） */

#define CHG_TEMP_OFFSET                 40

#define CHG_START_VALUE                 1u
#define CHG_STOP_VALUE                  0u

#define CHG_PILE_NORMAL                 0u
#define CHG_PILE_FAULT                  1u
#define CHG_PILE_ALARM                  2u

#define CHG_SS_START                    0u
#define CHG_SS_STANDBY                  1u
#define CHG_SS_STOP                     2u

#define CHG_POLL_MS                     100u
#define CHG_FAIL_OFFLINE                3u

void     charger_init         ( void );
void     charger_poll         ( void );
bool     charger_is_online    ( void );
bool     charger_cmd_start    ( void );
bool     charger_cmd_stop     ( void );
bool     charger_cmd_set_power( uint16_t power_x10 );   /* 写 1024，0.1kW */
bool     charger_cmd_write    ( uint16_t n, uint16_t value ); /* 仅 R/W 白名单 */
uint16_t charger_reg          ( uint16_t n );

#endif /* __CHARGER_H */
