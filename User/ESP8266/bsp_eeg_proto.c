/**
  ******************************************************************************
  * @file    bsp_eeg_proto.c
  * @brief   EEG V1.1 报文层实现（Doc/MQTT 网关通信协议 V1.1.md §5~§9）
  *
  * 与旧私有协议的关键差异：
  *   1. 主题从 /device/{id}/xxx 换成 eeg/{site}/{gw}/{type}/{id}/{channel}
  *   2. 数值从「整数倍率」换成带小数点的十进制（380.1 / 31.2 / 12.3）
  *      —— 这里手工拼小数，不用 %f：ARMCC 的浮点 printf 又大又慢，而且
  *         倍率整数本来就是精确值，转成 float 反而会引入舍入。
  *   3. 每条报文都带 ts（Unix 秒，SNTP 对时）
  *   4. §6 那条设备状态报文转义后整条 AT 命令 266 字节，超过乐鑫 256 上限，
  *      由 ESP8266_MQTT_Publish() 自动改走 AT+MQTTPUBRAW
  ******************************************************************************
  */
#include "./ESP8266/bsp_eeg_proto.h"
#include "./ESP8266/bsp_esp8266_test.h"
#include "./ESP8266/bsp_esp8266_mqtt.h"
#include "./ESP8266/bsp_esp8266.h"
#include "./devices/charger.h"
#include "./led/bsp_led.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================== 设备对象模型缓存 ================== */
uint16_t g_state_code            = 0;
uint16_t g_fault_code            = 0;
uint16_t g_enable                = 0;
uint16_t g_charge_power_setpoint = 0;
uint32_t g_energy_charge         = 0;
uint32_t g_energy_discharge      = 0;
uint8_t  g_soc                   = 0;
int16_t  g_temperature           = 25;
uint16_t g_mode_code             = 0;
uint16_t g_start_stop            = 0;
uint16_t g_start_stop_control    = 0;

/* 目标充电功率（0.1kW），§7 set_charge_power 写这里；接 Modbus 后要同时写
 * 寄存器 1025（充电输出功率），启停控制写寄存器 1051。 */
static uint32_t s_target_power_x10 = 65;    /* 默认 6.5kW */

/* 所有报文共用一块拼装缓冲：发布调用都在主循环里串行发生，不会重入。
 * 放 static 是因为工程栈很小（startup 里 Stack_Size），大数组不能上栈。 */
static char s_pl[320];

/* ================== 时间 ================== */
static uint32_t s_epoch_at_sync = 0;
static uint32_t s_tick_at_sync  = 0;
static uint32_t s_last_try_ms   = 0;
static bool     s_time_synced   = false;

static const char* const k_month[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* 民用日期 -> 1970-01-01 起的天数（Howard Hinnant days_from_civil，整数精确） */
static uint32_t days_from_civil(int y, int m, int d)
{
    int      era;
    unsigned yoe, doy, doe;

    y -= (m <= 2);
    era = y / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return (uint32_t)(era * 146097 + (int)doe - 719468);
}

static int scan_uint(const char** pp)
{
    const char* p = *pp;
    int v = 0;
    int digits = 0;

    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        digits++;
    }
    *pp = p;
    return digits ? v : -1;
}

/* +CIPSNTPTIME:Thu Aug 13 05:26:00 2026   （时区设成 0，所以这是 UTC） */
static bool parse_sntp_time(const char* buf, uint32_t* epoch)
{
    const char* p = strstr(buf, "+CIPSNTPTIME:");
    int day, hh, mm, ss, year, mon = -1, i;

    if (!p) return false;
    p += strlen("+CIPSNTPTIME:");

    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;        /* 星期几，跳过 */
    while (*p == ' ') p++;

    for (i = 0; i < 12; i++) {
        if (strncmp(p, k_month[i], 3) == 0) { mon = i + 1; break; }
    }
    if (mon < 0) return false;
    p += 3;

    day = scan_uint(&p);
    hh  = scan_uint(&p);
    if (*p != ':') return false;
    p++;
    mm = scan_uint(&p);
    if (*p != ':') return false;
    p++;
    ss   = scan_uint(&p);
    year = scan_uint(&p);

    if (day < 1 || hh < 0 || mm < 0 || ss < 0 || year < EEG_SNTP_MIN_VALID_YEAR) {
        return false;   /* 没对上时固件回的是 1970，这里一并挡掉 */
    }

    *epoch = days_from_civil(year, mon, day) * 86400UL
           + (uint32_t)hh * 3600UL + (uint32_t)mm * 60UL + (uint32_t)ss;
    return true;
}

void EEG_TimeInit(void)
{
    char cmd[128];

    snprintf(cmd, sizeof(cmd), "AT+CIPSNTPCFG=1,0,\"%s\",\"%s\"",
             EEG_SNTP_SERVER1, EEG_SNTP_SERVER2);
    if (!ESP8266_Cmd(cmd, "OK", 0, 2000)) {
        printf("[EEG SNTP] CIPSNTPCFG rejected - ts will fall back to uptime\r\n");
        return;
    }
    printf("[EEG SNTP] configured (tz=0/UTC, %s, %s)\r\n", EEG_SNTP_SERVER1, EEG_SNTP_SERVER2);
}

bool EEG_TimeSync(void)
{
    uint32_t epoch = 0;

    s_last_try_ms = HAL_GetTick();

    if (!ESP8266_Cmd("AT+CIPSNTPTIME?", "OK", 0, 2000)) return false;
    if (!parse_sntp_time(strEsp8266_Fram_Record.Data_RX_BUF, &epoch)) return false;

    s_epoch_at_sync = epoch;
    s_tick_at_sync  = HAL_GetTick();
    s_time_synced   = true;
    printf("[EEG SNTP] time synced, epoch=%lu\r\n", (unsigned long)epoch);
    return true;
}

void EEG_TimeTask(void)
{
    uint32_t now = HAL_GetTick();

    if (!s_time_synced) {
        if ((now - s_last_try_ms) >= 60000UL) EEG_TimeSync();   /* 没对上：1 分钟重试 */
        return;
    }
    if ((now - s_tick_at_sync) >= EEG_SNTP_RESYNC_MS) EEG_TimeSync();
}

uint32_t EEG_Timestamp(void)
{
    uint32_t now = HAL_GetTick();

    if (!s_time_synced) return now / 1000UL;             /* 退化：开机秒数 */
    return s_epoch_at_sync + (now - s_tick_at_sync) / 1000UL;
}

bool EEG_TimeIsSynced(void)
{
    return s_time_synced;
}

/* ================== 小工具 ================== */

/* 倍率整数 -> 定点小数字符串，dec = 小数位数（1 或 2） */
static void fmt_scaled(char* out, int outsz, uint32_t scaled, int dec)
{
    if (dec == 2) {
        snprintf(out, outsz, "%lu.%02lu",
                 (unsigned long)(scaled / 100), (unsigned long)(scaled % 100));
    } else {
        snprintf(out, outsz, "%lu.%lu",
                 (unsigned long)(scaled / 10), (unsigned long)(scaled % 10));
    }
}

/* §6 state 为业务派生值，原始寄存器码单独放在 state_code。 */
static const char* state_str(void)
{
    if (g_onoff_state == ONOFF_OFFLINE) return "offline";
    if (g_state_code == CHG_STATE_FAULT || g_fault_code != 0) return "fault";
    if (g_state_code == CHG_STATE_ALARM) return "alarm";
    switch (g_onoff_state) {
        case ONOFF_CHARGING: return "charging";
        default:             return "idle";
    }
}

static const char* mode_str(void)
{
    return (g_mode_code == 1u) ? "v2g" : "chg";
}

static bool device_online(void)
{
#if MB_MASTER_ENABLE
    return charger_is_online();
#else
    return true;
#endif
}

/* §8 的 id 要原样回传：命令里是数字就不加引号，是字符串就补引号 */
static void fmt_id(const char* id_raw, char* out, int outsz)
{
    int i;
    bool numeric;

    if (!id_raw || !id_raw[0]) {
        snprintf(out, outsz, "0");
        return;
    }
    numeric = true;
    for (i = 0; id_raw[i]; i++) {
        if (id_raw[i] < '0' || id_raw[i] > '9') { numeric = false; break; }
    }
    if (numeric) snprintf(out, outsz, "%s", id_raw);
    else         snprintf(out, outsz, "\"%s\"", id_raw);
}

#if !MB_MASTER_ENABLE
/* 充电时按 P = U × I 反推电流，免得三个数互相打架 */
static void apply_sim_power(void)
{
    g_current_voltage = 2210;                                  /* 221.0V */
    g_current_power   = s_target_power_x10;
    g_current_current = (g_current_voltage > 0)
                      ? (s_target_power_x10 * 100000UL / g_current_voltage)
                      : 0;
}
#endif

/* ================== §5 网关状态 ================== */

static void query_ip(char* out, int outsz)
{
    const char* p;
    int i = 0;

    out[0] = '\0';
    if (!ESP8266_Cmd("AT+CIPSTA?", "OK", 0, 1500)) return;

    p = strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+CIPSTA:ip:\"");
    if (!p) return;
    p += strlen("+CIPSTA:ip:\"");
    while (i < outsz - 1 && p[i] && p[i] != '"') { out[i] = p[i]; i++; }
    out[i] = '\0';
}

/* +CWJAP:"ssid","bssid",channel,rssi,... */
static int query_rssi(void)
{
    const char* p;
    int i;

    if (!ESP8266_Cmd("AT+CWJAP?", "OK", 0, 2000)) return 0;

    p = strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+CWJAP:");
    if (!p) return 0;
    p += strlen("+CWJAP:");

    for (i = 0; i < 2; i++) {                 /* 跳过 ssid 和 bssid 两个带引号字段 */
        p = strchr(p, '"'); if (!p) return 0; p++;
        p = strchr(p, '"'); if (!p) return 0; p++;
    }
    if (*p == ',') p++;
    while (*p && *p != ',') p++;              /* channel */
    if (*p == ',') p++;

    return atoi(p);
}

bool EEG_PublishGatewayStatus(void)
{
    char ip[20];

    query_ip(ip, sizeof(ip));

    snprintf(s_pl, sizeof(s_pl),
        "{\"online\":true,\"fw\":\"%s\",\"hw\":\"%s\",\"ip\":\"%s\","
        "\"rssi\":%d,\"uptime\":%lu,\"ts\":%lu}",
        EEG_FW_VERSION, EEG_HW_VERSION, ip[0] ? ip : "0.0.0.0",
        query_rssi(),
        (unsigned long)(HAL_GetTick() / 1000UL),
        (unsigned long)EEG_Timestamp());

    if (!ESP8266_MQTT_Publish(EEG_TOPIC_GW_STATUS, s_pl,
                              MQTT_DEFAULT_QOS, EEG_RETAIN_STATUS)) {
        printf("[EEG GW] publish FAILED: %s\r\n", s_pl);
        return false;
    }
    printf("[EEG GW] %s\r\n", s_pl);
    return true;
}

/* ================== §6 设备状态 ================== */

bool EEG_PublishDeviceStatus(void)
{
    char v[16], a[16], p[16], echg[16], edis[16];

    fmt_scaled(v, sizeof(v), g_current_voltage, 1);   /* 0.1V  -> 221.0 */
    fmt_scaled(a, sizeof(a), g_current_current, 2);   /* 0.01A -> 29.41 */
    fmt_scaled(p, sizeof(p), g_current_power,   1);   /* 0.1kW -> 6.5   */
    fmt_scaled(echg, sizeof(echg), g_energy_charge,    1);   /* 0.1kWh  -> 12.3  */
    fmt_scaled(edis, sizeof(edis), g_energy_discharge, 2);   /* 0.01kWh -> 0.00  */

    snprintf(s_pl, sizeof(s_pl),
        "{\"online\":%s,\"state\":\"%s\",\"state_code\":%u,\"fault_code\":%u,"
        "\"enable\":%u,\"voltage\":%s,\"current\":%s,\"power\":%s,"
        "\"energy_charge\":%s,\"energy_discharge\":%s,"
        "\"soc\":%u,\"temperature\":%d,\"mode\":\"%s\",\"mode_code\":%u,"
        "\"start_stop\":%u,\"start_stop_control\":%u,\"ts\":%lu}",
        device_online() ? "true" : "false",
        state_str(), (unsigned)g_state_code, (unsigned)g_fault_code,
        (unsigned)g_enable, v, a, p, echg, edis,
        (unsigned)g_soc, (int)g_temperature, mode_str(), (unsigned)g_mode_code,
        (unsigned)g_start_stop, (unsigned)g_start_stop_control,
        (unsigned long)EEG_Timestamp());

    if (!ESP8266_MQTT_Publish(EEG_TOPIC_DEV_STATUS, s_pl,
                              MQTT_DEFAULT_QOS, EEG_RETAIN_STATUS)) {
        printf("[EEG DEV] publish FAILED: %s\r\n", s_pl);
        return false;
    }
    printf("[EEG DEV] %s\r\n", s_pl);
    return true;
}

/* ================== §8 ACK ================== */

bool EEG_PublishAck(const char* id_raw, bool ok, int code, const char* msg)
{
    char id[40];

    fmt_id(id_raw, id, sizeof(id));
    snprintf(s_pl, sizeof(s_pl),
        "{\"id\":%s,\"result\":\"%s\",\"code\":%d,\"msg\":\"%s\",\"ts\":%lu}",
        id, ok ? "ok" : "error", code, msg ? msg : "",
        (unsigned long)EEG_Timestamp());

    if (!ESP8266_MQTT_Publish(EEG_TOPIC_DEV_ACK, s_pl,
                              MQTT_DEFAULT_QOS, EEG_RETAIN_TRANSIENT)) {
        printf("[EEG ACK] publish FAILED: %s\r\n", s_pl);
        return false;
    }
    printf("[EEG ACK] %s\r\n", s_pl);
    return true;
}

/* ================== §9 事件 ================== */

bool EEG_PublishEvent(const char* level, int code, const char* msg, int temperature)
{
    snprintf(s_pl, sizeof(s_pl),
        "{\"level\":\"%s\",\"code\":%d,\"msg\":\"%s\",\"temperature\":%d,\"ts\":%lu}",
        level ? level : "info", code, msg ? msg : "", temperature,
        (unsigned long)EEG_Timestamp());

    if (!ESP8266_MQTT_Publish(EEG_TOPIC_DEV_EVENT, s_pl,
                              MQTT_DEFAULT_QOS, EEG_RETAIN_TRANSIENT)) {
        printf("[EEG EVT] publish FAILED: %s\r\n", s_pl);
        return false;
    }
    printf("[EEG EVT] %s\r\n", s_pl);
    return true;
}

/* ================== §7 控制命令 ================== */

bool EEG_HandleCommand(const char* json)
{
    char id_raw[32] = {0};
    char action[32] = {0};
    int  value = 0;

    if (!MQTT_JsonGetText(json, "id", id_raw, sizeof(id_raw))) id_raw[0] = '\0';

    if (!MQTT_JsonGetStr(json, "action", action, sizeof(action))) {
        printf("\r\n[EEG CMD] no \"action\" field - dropped\r\n");
        return false;
    }

    /* start：写保持寄存器 1051。MQTT 组包/ACK 路径不变。 */
    if (strcmp(action, "start") == 0) {
#if MB_MASTER_ENABLE
        if (!charger_cmd_start()) {
            printf("\r\n[EEG CMD] start FAILED (modbus write 1051)\r\n");
            return EEG_PublishAck(id_raw, false, 3, "modbus write failed");
        }
#else
        apply_sim_power();
        g_onoff_state = ONOFF_CHARGING;
        g_start_stop_control = CHG_CTRL_START_VALUE;
        LED2_ON;
#endif
        printf("\r\n[EEG CMD] start -> charging (LED2 ON)\r\n");
        return EEG_PublishAck(id_raw, true, 0, "started");
    }

    if (strcmp(action, "stop") == 0) {
#if MB_MASTER_ENABLE
        if (!charger_cmd_stop()) {
            printf("\r\n[EEG CMD] stop FAILED (modbus write 1051)\r\n");
            return EEG_PublishAck(id_raw, false, 3, "modbus write failed");
        }
#else
        g_onoff_state     = ONOFF_IDLE;
        g_start_stop_control = CHG_CTRL_STOP_VALUE;
        g_current_power   = 0;
        g_current_current = 0;
        g_current_voltage = 2200;
        LED2_OFF;
#endif
        printf("\r\n[EEG CMD] stop -> idle (LED2 OFF)\r\n");
        return EEG_PublishAck(id_raw, true, 0, "stopped");
    }

    /* set_charge_power：value 单位 kW，写寄存器 1025（0.1kW） */
    if (strcmp(action, "set_charge_power") == 0) {
        if (!MQTT_JsonGetInt(json, "value", &value) || value < 0 || value > 250) {
            printf("\r\n[EEG CMD] set_charge_power value invalid\r\n");
            return EEG_PublishAck(id_raw, false, 2, "invalid value");
        }
        s_target_power_x10 = (uint32_t)value * 10U;
        g_charge_power_setpoint = (uint16_t)s_target_power_x10;
#if MB_MASTER_ENABLE
        if (!charger_cmd_set_power((uint16_t)s_target_power_x10)) {
            printf("\r\n[EEG CMD] set_charge_power FAILED (modbus write 1025)\r\n");
            return EEG_PublishAck(id_raw, false, 3, "modbus write failed");
        }
#else
        if (g_onoff_state == ONOFF_CHARGING) apply_sim_power();
#endif
        printf("\r\n[EEG CMD] set_charge_power = %d kW\r\n", value);
        return EEG_PublishAck(id_raw, true, 0, "power set");
    }

    printf("\r\n[EEG CMD] unsupported action=%s\r\n", action);
    return EEG_PublishAck(id_raw, false, 1, "unsupported action");
}

/* ================== 派生量 ================== */

void EEG_UpdateDerived(void)
{
    static uint32_t last_ms   = 0;
    static uint32_t energy_rem = 0;
    static uint8_t  soc_tick   = 0;
    uint32_t now = HAL_GetTick();
    uint32_t dt;

    if (last_ms == 0) { last_ms = now; return; }
    dt      = now - last_ms;
    last_ms = now;

    if (g_onoff_state != ONOFF_CHARGING) return;

    /* 电量积分：0.1kWh 增量 = P(0.1kW) × dt(ms) / 3600000，余数留到下一轮，
     * 免得每次都往下取整把电量吃掉。 */
    if (g_current_power > 0) {
        uint32_t num = g_current_power * dt + energy_rem;
        g_energy_charge += num / 3600000UL;
        energy_rem       = num % 3600000UL;
    }

    /* SOC 目前没有 BMS 数据源，先按每 6 个上报周期涨 1% 顶着，
     * 等 Modbus BMS 接进来直接换成真实寄存器值。 */
    if (++soc_tick >= 6) {
        soc_tick = 0;
        if (g_soc < 100) g_soc++;
    }
}

void EEG_CheckTempAlarm(void)
{
    static bool alarm = false;

    if (!alarm && g_temperature >= EEG_TEMP_ALARM_C) {
        alarm        = true;
        EEG_PublishEvent("alarm", EEG_EVT_CODE_OVER_TEMP, "over temperature", g_temperature);
    } else if (alarm && g_temperature <= EEG_TEMP_CLEAR_C) {
        alarm        = false;
        EEG_PublishEvent("info", EEG_EVT_CODE_OVER_TEMP, "over temperature cleared", g_temperature);
    }
}
