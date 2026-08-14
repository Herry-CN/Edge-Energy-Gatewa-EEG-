#ifndef __CHARGER_H
#define __CHARGER_H

/**
  ******************************************************************************
  * @file    charger.h
  * @brief   充电桩设备层：Modbus 寄存器 ↔ EEG 对象模型缓存
  *
 * 寄存器表见 Doc/Modbus 设备模型规范 V1.1.md。
  * 换品牌只改本文件的地址/倍率宏，MQTT 报文层不用动。
  ******************************************************************************
  */

#include <stdint.h>
#include <stdbool.h>

#define MB_MASTER_ENABLE                1

/* PC 端 Modbus Server 从站号，与软件里 Slave ID 一致 */
#define CHG_SLAVE_ADDR                  1u

/*
 * 文档里的 1002/1035/1050/1051 属于 1-based 简写地址；真正进 Modbus PDU 的是零基地址，
 * 所以报文里统一按「文档地址 - 1」发送。
 */
#define CHG_REG_OFFSET                  (-1)

#define CHG_REG_STATE                   1002
#define CHG_REG_FAULT                   1003
#define CHG_REG_ENABLE                  1024
#define CHG_REG_CHARGE_POWER            1025    /* 写：目标功率 0.1kW */
#define CHG_REG_VOLTAGE                 1029    /* 0.1V   → g_current_voltage */
#define CHG_REG_CURRENT                 1030    /* 0.01A  → g_current_current */
#define CHG_REG_POWER                   1031    /* 0.1kW  → g_current_power   */
#define CHG_REG_SOC                     1032    /* 1%     */
#define CHG_REG_START_STOP              1035    /* 读：启停状态 */
#define CHG_REG_ENERGY_CHARGE           1036    /* 0.1kWh */
#define CHG_REG_ENERGY_DISCHARGE        1038    /* 0.01kWh */
#define CHG_REG_TEMPERATURE             1040    /* 实际温度 = raw - 40 */
#define CHG_REG_MODE                    1050
#define CHG_REG_START_STOP_CTRL         1051    /* 写：启停控制 */

#define CHG_STATUS_STARTED              0u
#define CHG_STATUS_STANDBY              1u
#define CHG_STATUS_STOPPED              2u

#define CHG_CTRL_START_VALUE            1u
#define CHG_CTRL_STOP_VALUE             0u

#define CHG_STATE_NORMAL                0u
#define CHG_STATE_FAULT                 1u
#define CHG_STATE_ALARM                 2u

#define CHG_POLL_MS                     300u
#define CHG_OFFLINE_PROBE_MS            3000u
#define CHG_FAIL_OFFLINE                3u
#define CHG_RECOVER_ONLINE              2u

void charger_init        ( void );
void charger_poll        ( void );
bool charger_is_online   ( void );
bool charger_cmd_start   ( void );
bool charger_cmd_stop    ( void );
bool charger_cmd_set_power(uint16_t power_x10);   /* 0.1kW */

#endif /* __CHARGER_H */
