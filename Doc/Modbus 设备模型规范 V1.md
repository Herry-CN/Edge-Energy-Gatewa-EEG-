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
 └── enable
```

寄存器映射：

以后换品牌充电桩，只需要重新映射寄存器。

