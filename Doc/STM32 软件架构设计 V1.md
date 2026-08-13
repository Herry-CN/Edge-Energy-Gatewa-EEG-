# STM32 软件架构设计 V1

### 总体架构

```Plain Text
+------------------------------------------------+
|                  Application                   |
+----------------------+-------------------------+
| Device Manager       | MQTT Command Manager    |
+----------------------+-------------------------+
| Register Cache       | JSON Codec              |
+----------------------+-------------------------+
| Modbus Master        | ESP8266 AT Driver       |
+----------------------+-------------------------+
| UART RS485           | UART WiFi               |
+------------------------------------------------+
```

### 模块划分

### BSP

负责：

- UART1（ESP8266）

- UART2（RS485）

- GPIO

- Timer

- Watchdog

### Modbus

文件：

```Plain Text
modbus_master.c
modbus_crc.c
```

接口：

```Plain Text
int mb_read(uint8_t addr, uint16_t reg, uint16_t num);
int mb_write(uint8_t addr, uint16_t reg, uint16_t value);
```

### Register Cache

结构：

```Plain Text
typedef struct
{
    uint16_t state;           //1001
    uint16_t fault;           //1002
    uint16_t voltage;         //1028
    uint16_t current;         //1029
    uint16_t power;           //1030
    uint16_t soc;             //1031
    uint16_t energy_charge;   //1035
    uint16_t temperature;     //1039
    uint16_t mode;            //1049
} ChargerRegCache;
```

### Device Layer

目录：

```Plain Text
devices/
    charger.c
    battery.c
    inverter.c
```

统一接口：

```Plain Text
void device_init(void);
void device_poll(void);
void device_build_status_json(char *buf);
int device_execute_command(Command *cmd);
```

### MQTT Layer

STM32 不实现 MQTT。

只负责：

```Plain Text
mqtt_publish(topic, payload);
mqtt_subscribe(topic);
mqtt_process_rx();
```

底层通过：

```Plain Text
AT+MQTTPUB=...
```

### JSON Codec

建议使用：

cJSON

负责：

- build\_status\_json\(\)

- parse\_cmd\_json\(\)

