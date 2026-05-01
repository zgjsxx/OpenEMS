# OpenEMS 策略引擎说明

本文档说明当前 `openems-strategy-engine` 的定位、控制顺序、参数模型，以及已经支持的自动控制能力。

## 1. 模块定位

`openems-strategy-engine` 是运行在 OpenEMS 主链路之上的独立策略进程，职责是：

- 从 RtDb 读取实时点位
- 从 PostgreSQL 读取策略配置
- 计算自动控制目标
- 通过现有 command slot 下发遥控 / 遥调
- 把运行状态和动作日志写回 PostgreSQL

它不直接访问 Modbus / IEC104 驱动，而是复用现有采集链路和控制链路。

## 2. 当前支持的自动控制能力

当前固定支持三层顺序控制：

1. `AntiReverseFlow`
2. `SocProtection`
3. `PV Curtailment Compensation`

控制语义是：

- 防逆流先算站级目标
- SOC 保护对储能目标做裁剪
- 如果储能因高 SOC 或不可用无法继续消纳，再由光伏限功率补偿接管

## 3. 当前控制顺序

```text
Step 1: AntiReverseFlow
  根据并网点功率偏差计算储能总目标功率

Step 2: SocProtection
  根据 SOC 上下限对储能目标进行裁剪

Step 3: PV Curtailment Compensation
  当储能不能继续充电且并网点仍有逆流时，
  通过 pv-target-power-limit 对光伏进行限发
```

## 4. 多设备控制模型

当前已经支持多设备参与同一条站级策略链路。

### 4.1 多储能目标分配

策略引擎会先算出一个站级储能总目标功率，再分配到所有可参与的 BESS。

当前分配规则：

- 先过滤不可参与设备：
  - 停机
  - 运行状态异常
  - 被 SOC 上下限约束卡住
- 剩余可用 BESS 再按最大功率能力分配

分配原则：

- 当总目标为负值时，表示充电
  - 按 `bess_max_charge_kw#设备组` 分配
- 当总目标为正值时，表示放电
  - 按 `bess_max_discharge_kw#设备组` 分配
- 如果没有配置分设备能力参数，则回退到全局：
  - `max_charge_kw`
  - `max_discharge_kw`

示例：

- `bess_max_charge_kw#bess-001 = 60`
- `bess_max_charge_kw#bess-002 = 100`
- 站级充电目标 `-26kW`

则：

- `bess-001 = -26 × 60 / (60 + 100) = -9.75kW`
- `bess-002 = -26 × 100 / (60 + 100) = -16.25kW`

### 4.2 多光伏限功率

多光伏场景下，策略引擎按光伏额定功率比例进行限发。

当前实现方式：

- 每台 PV 单独配置额定功率
- 策略先计算站级需要保留的总光伏功率
- 再折算出统一的限发百分比
- 对所有参与 PV 下发同一个 `pv-target-power-limit`

这在数学上等价于：

- 每台 PV 按各自额定功率比例承担限发量

示例：

- `pv_rated_power_kw#pv-001 = 80`
- `pv_rated_power_kw#pv-002 = 120`
- 统一限发到 `5%`

则：

- `pv-001` 实际出力约 `4kW`
- `pv-002` 实际出力约 `6kW`

## 5. 绑定角色

策略绑定保存在 `strategy_bindings` 表里。

当前常用角色包括：

- `grid_power`
- `bess_power`
- `bess_soc`
- `bess_run_state`
- `bess_power_setpoint`
- `pv_power`
- `pv_power_limit_setpoint`
- `pv_run_state`

多设备场景下，绑定角色支持使用 `#组标识` 后缀，例如：

- `bess_power#bess-001`
- `bess_soc#bess-001`
- `bess_power_setpoint#bess-001`
- `bess_power#bess-002`
- `pv_power#pv-001`
- `pv_power_limit_setpoint#pv-002`

## 6. 参数模型

策略参数保存在 `strategy_params` 表里。

### 6.1 通用参数

- `export_limit_kw`
- `max_charge_kw`
- `max_discharge_kw`
- `deadband_kw`
- `ramp_rate_kw_per_s`
- `soc_low`
- `soc_high`
- `manual_override_minutes`

### 6.2 光伏限发参数

- `enable_pv_curtailment`
- `pv_rated_power_kw`
- `pv_limit_min_pct`
- `pv_limit_max_pct`
- `pv_limit_recovery_step_pct`

### 6.3 多设备能力参数

储能：

- `bess_max_charge_kw#bess-001`
- `bess_max_charge_kw#bess-002`
- `bess_max_discharge_kw#bess-001`
- `bess_max_discharge_kw#bess-002`

光伏：

- `pv_rated_power_kw#pv-001`
- `pv_rated_power_kw#pv-002`

说明：

- `pv_rated_power_kw` 可作为全局回退值
- 如配置了 `pv_rated_power_kw#组标识`，优先使用分设备额定功率
- 储能的“容量 kWh”不再参与目标功率分配，只用于仿真器里的 SOC 演化

## 7. 人工接管规则

系统保留“人工优先”规则。

当用户通过 Web 手动下发以下点位时：

- `bess-target-power`
- `pv-target-power-limit`
- 以及其他绑定在策略上的手动控制点

系统会把关联策略写入人工接管窗口。

当前默认规则：

- 接管时长：`30` 分钟
- 接管期间：策略不自动覆盖人工命令
- 到期后：策略自动恢复接管

## 8. 当前已验证的典型场景

### 场景一：双储能防逆流

条件：

- `pv-001 = 18kW`
- `pv-002 = 18kW`
- `load = 10kW`
- `bess-001 SOC = 50%`
- `bess-002 SOC = 55%`
- 储能最大可充功率：
  - `bess-001 = 60kW`
  - `bess-002 = 100kW`

预期：

- 总储能目标约 `-26kW`
- `bess-001` 约承担 `-9.75kW`
- `bess-002` 约承担 `-16.25kW`
- `grid-active-power` 回到 `0kW` 附近

### 场景二：高 SOC 时双光伏限发

条件：

- `pv-001 = 18kW`
- `pv-002 = 18kW`
- `load = 10kW`
- `bess-001 SOC = 90%`
- `bess-002 SOC = 92%`
- 光伏额定功率：
  - `pv-001 = 80kW`
  - `pv-002 = 120kW`

预期：

- 两台储能目标都被压为 `0`
- `pv-target-power-limit` 与 `pv2-target-power-limit` 同时生效
- `pv-001` 实际出力约 `4kW`
- `pv-002` 实际出力约 `6kW`
- `grid-active-power` 回到 `0kW` 附近

## 9. 当前边界

当前仍未覆盖：

- 多站点协同
- 不同设备健康度参与分配
- 需量控制
- 分时电价策略
- 通用表达式引擎
- 脚本化策略

## 10. 后续建议

建议后续继续扩展：

1. 低 SOC 禁止放电的 E2E 场景
2. 手动接管 30 分钟 E2E 场景
3. 一台 BESS 停机后另一台自动承担更多目标
4. 光伏限发恢复过程的长期稳定性验证
5. 基于健康度 / 故障 / 优先级的动态能力分配
