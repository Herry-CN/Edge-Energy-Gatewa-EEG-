/**
  ******************************************************************************
  * @file    bsp_config.c
  * @brief   网络配置 RAM 镜像 + Flash 末页持久化 + USART1 config CLI
  ******************************************************************************
  */
#include "./config/bsp_config.h"
#include "./ESP8266/bsp_esp8266_test.h"
#include "./ESP8266/bsp_esp8266.h"
#include "./wdg/bsp_iwdg.h"
#include "stm32f1xx_hal_flash.h"
#include "stm32f1xx_hal_flash_ex.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef char eeg_cfg_even_sz[((sizeof(EegNetConfig) % 2u) == 0u) ? 1 : -1];

static EegNetConfig s_cfg;
static uint8_t      s_from_flash = 0;
static uint8_t      s_log_verbose = 1;   /* 启动阶段开，连上 MQTT 后主循环关掉 */
static uint8_t      s_apply_pending = 0;

static uint32_t crc32_ieee(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i, b;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8u; b++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static void copy_str(char *dst, uint32_t dstsz, const char *src)
{
    uint32_t i = 0;

    if (dst == NULL || dstsz == 0u) return;
    if (src == NULL) src = "";
    while (src[i] != '\0' && (i + 1u) < dstsz) {
        dst[i] = src[i];
        i++;
    }
    while (i < dstsz) {
        dst[i++] = '\0';
    }
}

static uint32_t cfg_crc(const EegNetConfig *c)
{
    EegNetConfig tmp;

    tmp = *c;
    tmp.crc = 0;
    return crc32_ieee((const uint8_t *)&tmp, sizeof(tmp));
}

static int cfg_valid(const EegNetConfig *c)
{
    if (c->magic != EEG_CFG_MAGIC) return 0;
    if (c->version != EEG_CFG_VERSION) return 0;
    if (c->mqtt_port == 0u) return 0;
    if (c->wifi_ssid[0] == '\0') return 0;
    if (c->mqtt_host[0] == '\0') return 0;
    if (cfg_crc(c) != c->crc) return 0;
    return 1;
}

void Config_ResetDefaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic     = EEG_CFG_MAGIC;
    s_cfg.version   = EEG_CFG_VERSION;
    s_cfg.mqtt_port = (uint16_t)MQTT_PORT;
    copy_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), macUser_ESP8266_ApSsid);
    copy_str(s_cfg.wifi_pwd,  sizeof(s_cfg.wifi_pwd),  macUser_ESP8266_ApPwd);
    copy_str(s_cfg.mqtt_host, sizeof(s_cfg.mqtt_host), MQTT_BROKERADDRESS);
    copy_str(s_cfg.mqtt_user, sizeof(s_cfg.mqtt_user), MQTT_USER_NAME);
    copy_str(s_cfg.mqtt_pass, sizeof(s_cfg.mqtt_pass), MQTT_PASSWD);
    s_cfg.crc = cfg_crc(&s_cfg);
    s_from_flash = 0;
}

void Config_Load(void)
{
    const EegNetConfig *rom = (const EegNetConfig *)EEG_CFG_FLASH_ADDR;

    if (cfg_valid(rom)) {
        s_cfg = *rom;
        s_from_flash = 1;
        printf("[CFG] loaded from Flash @ 0x%08lX  wifi=%s  mqtt=%s:%u\r\n",
               (unsigned long)EEG_CFG_FLASH_ADDR,
               s_cfg.wifi_ssid, s_cfg.mqtt_host, (unsigned)s_cfg.mqtt_port);
        return;
    }

    Config_ResetDefaults();
    printf("[CFG] Flash empty/invalid -> firmware defaults  wifi=%s  mqtt=%s:%u\r\n",
           s_cfg.wifi_ssid, s_cfg.mqtt_host, (unsigned)s_cfg.mqtt_port);
}

bool Config_Save(void)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t page_err = 0xFFFFFFFFu;
    HAL_StatusTypeDef st;
    uint32_t addr;
    const uint16_t *hw;
    uint32_t n, i;

    s_cfg.magic   = EEG_CFG_MAGIC;
    s_cfg.version = EEG_CFG_VERSION;
    s_cfg.crc     = cfg_crc(&s_cfg);

    if (!cfg_valid(&s_cfg)) {
        printf("[CFG] save aborted: in-RAM config invalid\r\n");
        return false;
    }

    IWDG_Feed();
    HAL_FLASH_Unlock();

    memset(&erase, 0, sizeof(erase));
    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = EEG_CFG_FLASH_ADDR;
    erase.NbPages     = 1;
#if defined(FLASH_BANK2_END)
    /* F103ZE HAL 把 512KB 都算 Bank1（FLASH_BANK1_END=0x0807FFFF）。 */
    erase.Banks = FLASH_BANK_1;
#endif
    st = HAL_FLASHEx_Erase(&erase, &page_err);
    IWDG_Feed();
    if (st != HAL_OK) {
        HAL_FLASH_Lock();
        printf("[CFG] erase FAIL st=%d page_err=0x%08lX\r\n",
               (int)st, (unsigned long)page_err);
        return false;
    }

    hw = (const uint16_t *)&s_cfg;
    n  = (sizeof(s_cfg) + 1u) / 2u;
    addr = EEG_CFG_FLASH_ADDR;
    for (i = 0; i < n; i++) {
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, hw[i]);
        if (st != HAL_OK) {
            HAL_FLASH_Lock();
            printf("[CFG] program FAIL @ 0x%08lX\r\n", (unsigned long)addr);
            return false;
        }
        addr += 2u;
    }
    HAL_FLASH_Lock();
    IWDG_Feed();

    if (!cfg_valid((const EegNetConfig *)EEG_CFG_FLASH_ADDR)) {
        printf("[CFG] verify FAIL after write\r\n");
        return false;
    }

    s_from_flash = 1;
    printf("[CFG] saved to Flash\r\n");
    return true;
}

const EegNetConfig *Config_Get(void)
{
    return &s_cfg;
}

bool Config_FromFlash(void)
{
    return s_from_flash ? true : false;
}

uint8_t Config_LogVerbose(void)
{
    return s_log_verbose;
}

void Config_SetLogVerbose(uint8_t on)
{
    s_log_verbose = on ? 1u : 0u;
}

static const char *mask_secret(const char *s)
{
    return (s != NULL && s[0] != '\0') ? "****" : "(empty)";
}

void Config_PrintShow(void)
{
    printf("\r\n\r\n========== CONFIG SHOW ==========\r\n"
           "  source          = %s\r\n"
           "  wifi.ssid       = %s\r\n"
           "  wifi.password   = %s\r\n"
           "  mqtt.host       = %s\r\n"
           "  mqtt.port       = %u\r\n"
           "  mqtt.user       = %s\r\n"
           "  mqtt.password   = %s\r\n"
           "========== END CONFIG ==========\r\n\r\n",
           s_from_flash ? "Flash" : "defaults, not saved",
           s_cfg.wifi_ssid,
           mask_secret(s_cfg.wifi_pwd),
           s_cfg.mqtt_host,
           (unsigned)s_cfg.mqtt_port,
           s_cfg.mqtt_user,
           mask_secret(s_cfg.mqtt_pass));
}

void Config_PrintHelp(void)
{
    printf("\r\nUSART1 CLI (115200 8-N-1, Enter to send):\r\n"
           "  config show\r\n"
           "  config set wifi.ssid <ssid>\r\n"
           "  config set wifi.password <pwd>\r\n"
           "  config set mqtt.host <ip-or-host>\r\n"
           "  config set mqtt.port <1-65535>\r\n"
           "  config set mqtt.user <user>\r\n"
           "  config set mqtt.password <pwd>\r\n"
           "  config save              write Flash (survives power-off)\r\n"
           "  config apply             save + reconnect WiFi/MQTT\r\n"
           "  config reset             restore firmware defaults (RAM; save to persist)\r\n"
           "  log quiet                mute Modbus/AT/JSON dumps (default after online)\r\n"
           "  log verbose              restore those dumps\r\n"
           "  help\r\n\r\n");
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return s;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        e--;
    }
    *e = '\0';
    return s;
}

static int set_key(const char *key, const char *val)
{
    if (strcmp(key, "wifi.ssid") == 0) {
        if (val[0] == '\0') return -1;
        copy_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), val);
        return 0;
    }
    if (strcmp(key, "wifi.password") == 0) {
        copy_str(s_cfg.wifi_pwd, sizeof(s_cfg.wifi_pwd), val);
        return 0;
    }
    if (strcmp(key, "mqtt.host") == 0) {
        if (val[0] == '\0') return -1;
        copy_str(s_cfg.mqtt_host, sizeof(s_cfg.mqtt_host), val);
        return 0;
    }
    if (strcmp(key, "mqtt.port") == 0) {
        int p = atoi(val);
        if (p < 1 || p > 65535) return -1;
        s_cfg.mqtt_port = (uint16_t)p;
        return 0;
    }
    if (strcmp(key, "mqtt.user") == 0) {
        copy_str(s_cfg.mqtt_user, sizeof(s_cfg.mqtt_user), val);
        return 0;
    }
    if (strcmp(key, "mqtt.password") == 0) {
        copy_str(s_cfg.mqtt_pass, sizeof(s_cfg.mqtt_pass), val);
        return 0;
    }
    return -2;
}

/* returns 1 = caller should reconnect; 0 = handled / ignore */
static int handle_line(char *line)
{
    char *p;
    char *key;
    char *val;
    int   rc;

    line = trim(line);
    if (line[0] == '\0') return 0;

    /* 任意 CLI 行先关掉业务刷屏，避免 config show 被下一帧 AT/Modbus 冲掉。
     * log verbose 除外。 */
    if (strcmp(line, "log verbose") == 0 || strcmp(line, "log on") == 0) {
        Config_SetLogVerbose(1);
        printf("[LOG] verbose  (Modbus/AT/JSON dumps on)\r\n");
        return 0;
    }
    if (strcmp(line, "log quiet") == 0 || strcmp(line, "log off") == 0 ||
        strcmp(line, "log") == 0) {
        if (strcmp(line, "log") == 0) {
            printf("[LOG] %s  (log quiet / log verbose)\r\n",
                   s_log_verbose ? "verbose" : "quiet");
            return 0;
        }
        Config_SetLogVerbose(0);
        printf("[LOG] quiet  (CLI only; log verbose to restore dumps)\r\n");
        return 0;
    }

    Config_SetLogVerbose(0);

    if (strcmp(line, "help") == 0 || strcmp(line, "config") == 0 ||
        strcmp(line, "config help") == 0) {
        Config_PrintHelp();
        return 0;
    }
    if (strcmp(line, "config show") == 0) {
        Config_PrintShow();
        return 0;
    }
    if (strcmp(line, "config save") == 0) {
        Config_Save();
        return 0;
    }
    if (strcmp(line, "config apply") == 0) {
        if (!Config_Save()) return 0;
        printf("[CFG] applying: reconnect WiFi + MQTT ...\r\n");
        return 1;
    }
    if (strcmp(line, "config reset") == 0) {
        Config_ResetDefaults();
        printf("[CFG] RAM restored to firmware defaults. config save to persist.\r\n");
        Config_PrintShow();
        return 0;
    }

    if (strncmp(line, "config set ", 11) != 0) {
        printf("[CFG] unknown: %s  (type help)\r\n", line);
        return 0;
    }

    p = trim(line + 11);
    key = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) {
        *p++ = '\0';
        val = trim(p);
    } else {
        val = p;
    }

    rc = set_key(key, val);
    if (rc == 0) {
        s_from_flash = 0;
        printf("[CFG] set %s  (config save to keep after power-off)\r\n", key);
    } else if (rc == -2) {
        printf("[CFG] unknown key: %s\r\n", key);
    } else {
        printf("[CFG] invalid value for %s\r\n", key);
    }
    return 0;
}

bool Config_CliPoll(void)
{
    Config_CliService();
    if (s_apply_pending) {
        s_apply_pending = 0;
        return true;
    }
    return false;
}

void Config_CliService(void)
{
    char line[192];
    uint16_t n;

    if (strUSART_Fram_Record.InfBit.FramFinishFlag == 0u) {
        return;
    }

    __disable_irq();
    n = strUSART_Fram_Record.InfBit.FramLength;
    if (n >= sizeof(line)) n = (uint16_t)(sizeof(line) - 1u);
    memcpy(line, strUSART_Fram_Record.Data_RX_BUF, n);
    line[n] = '\0';
    strUSART_Fram_Record.InfBit.FramLength     = 0;
    strUSART_Fram_Record.InfBit.FramFinishFlag = 0;
    strUSART_Fram_Record.Data_RX_BUF[0]        = '\0';
    __enable_irq();

    if (n > 0u) {
        printf("\r\n>>> %s\r\n", line);
    }
    if (handle_line(line)) {
        s_apply_pending = 1;
    }
}
