#ifndef __CHARGER_H
#define __CHARGER_H

/**
  ******************************************************************************
  * @file    charger.h
  * @brief   充电桩设备层：Modbus 寄存器 ↔ EEG 对象模型缓存
  *
  * 寄存器表见 Doc/Modbus 设备模型规范 V1.md。
  * 换品牌只改本文件的地址/倍率宏，MQTT 报文层不用动。
  ******************************************************************************
  */

#include <stdint.h>
#include <stdbool.h>

#define MB_MASTER_ENABLE                1

/* PC 端 Modbus Server 从站号，与软件里 Slave ID 一致 */
#define CHG_SLAVE_ADDR                  1u

/*
 * PDU 地址 = 规范表里的寄存器号。
 * 若 Modbus Server 勾了 “PLC 地址 / 1-based”（界面 1001 = 报文 1000），
 * 把 CHG_REG_OFFSET 改成 -1。
 */
#define CHG_REG_OFFSET                  0

#define CHG_REG_STATE                   1001
#define CHG_REG_FAULT                   1002
#define CHG_REG_ENABLE                  1023
#define CHG_REG_CHARGE_POWER            1024    /* 写：目标功率 0.1kW */
#define CHG_REG_VOLTAGE                 1028    /* 0.1V   → g_current_voltage */
#define CHG_REG_CURRENT                 1029    /* 0.01A  → g_current_current */
#define CHG_REG_POWER                   1030    /* 0.1kW  → g_current_power   */
#define CHG_REG_SOC                     1031    /* 1%     */
#define CHG_REG_ENERGY_CHARGE           1035    /* uint32 LE，0.01kWh，占 1035/1036 */
#define CHG_REG_ENERGY_DISCHARGE        1037
#define CHG_REG_TEMPERATURE             1039    /* int16 ℃ */
#define CHG_REG_MODE                    1049
#define CHG_REG_START_STOP              1050    /* 写：启停 */

#define CHG_START_VALUE                 1u
#define CHG_STOP_VALUE                  0u

#define CHG_STATE_IDLE                  0u
#define CHG_STATE_CHARGING              1u
#define CHG_STATE_OFFLINE               2u

#define CHG_POLL_MS                     300u
#define CHG_FAIL_OFFLINE                3u

void charger_init        ( void );
void charger_poll        ( void );
bool charger_is_online   ( void );
bool charger_cmd_start   ( void );
bool charger_cmd_stop    ( void );
bool charger_cmd_set_power(uint16_t power_x10);   /* 0.1kW */

#endif /* __CHARGER_H */
