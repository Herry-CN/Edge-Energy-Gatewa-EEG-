#ifndef __BSP_CONFIG_H
#define __BSP_CONFIG_H

/**
  ******************************************************************************
  * @file    bsp_config.h
  * @brief   现场可改的网络参数：STM32 Flash 掉电保存 + USART1 CLI
  *
  * 编译宏（bsp_esp8266_test.h）只作出厂默认。运行时一律走 Config_Get()。
  ******************************************************************************
  */

#include "stm32f1xx.h"
#include <stdbool.h>
#include <stdint.h>

#define EEG_CFG_MAGIC           0x31474545u   /* 'EEG1' */
#define EEG_CFG_VERSION         1u

/* F103ZE 512KB，页 2KB。末页不进程序映像（当前固件约 25KB）。 */
#define EEG_CFG_FLASH_ADDR      0x0807F800u
#define EEG_CFG_FLASH_PAGE_SIZE 0x800u

#define EEG_CFG_SSID_LEN        32u
#define EEG_CFG_PWD_LEN         64u
#define EEG_CFG_HOST_LEN        64u
#define EEG_CFG_USER_LEN        32u
#define EEG_CFG_PASS_LEN        32u

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t mqtt_port;
    uint32_t crc;
    char     wifi_ssid[EEG_CFG_SSID_LEN];
    char     wifi_pwd[EEG_CFG_PWD_LEN];
    char     mqtt_host[EEG_CFG_HOST_LEN];
    char     mqtt_user[EEG_CFG_USER_LEN];
    char     mqtt_pass[EEG_CFG_PASS_LEN];
} EegNetConfig;

void                Config_Load     ( void );
bool                Config_Save     ( void );
void                Config_ResetDefaults( void );
const EegNetConfig *Config_Get      ( void );
bool                Config_FromFlash( void );

/* USART1：有一行则解析。config apply 返回 true，由调用方重连网络。 */
bool                Config_CliPoll  ( void );
void                Config_CliService( void );  /* 只解析，不消费 apply（AT 等待中可调） */
void                Config_PrintHelp( void );
void                Config_PrintShow( void );

/* 1 = 打印 Modbus 收发 / poll / EEG JSON / AT RX；0 = 只留 CLI 和故障。 */
uint8_t             Config_LogVerbose( void );
void                Config_SetLogVerbose( uint8_t on );

#endif /* __BSP_CONFIG_H */
