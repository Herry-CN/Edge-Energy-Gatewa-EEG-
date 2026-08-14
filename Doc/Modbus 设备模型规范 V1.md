# Modbus 设备模型规范 V1

### 设备对象模型

### Charger

```Plain Text
Charger
 ├── state
 ├── fault_code
 ├── voltage
 ├── current
 ├── power
 ├── energy_charge
 ├── energy_discharge
 ├── soc
 ├── temperature
 ├── mode
 ├── enable
 └── start_stop
```

| 模型字段        | 寄存器 |
| --------------- | ------ |
| state           | 1001   |
| fault_code      | 1002   |
| voltage         | 1028   |
| current         | 1029   |
| power           | 1030   |
| soc             | 1031   |
| energy_charge   | 1035   |
| energy_discharge| 1037   |
| temperature     | 1039   |
| mode            | 1049   |
| enable          | 1023   |
| start_stop      | 1050   |



以后换品牌充电桩，只需要重新映射寄存器。

