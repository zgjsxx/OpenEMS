# OpenEMS 策略引擎说明

本文档说明当前 `openems-strategy-engine` 的定位、控制顺序、数据库配置项，以及当前已经支持的自动控制能力。

## 1. 模块定位

`openems-strategy-engine` 是运行在 OpenEMS 主链路之上的独立策略进程。

它的职责是：

- 从 RtDb 读取实时点位
- 从 PostgreSQL 读取策略配置
- 计算自动控制目标
- 通过现有 command slot 下发遥控 / 遥调
- 把运行状态和动作日志写回 PostgreSQL

它不直接访问 Modbus/IEC104 驱动，而是复用现有实时链路和控制链路。

## 2. 当前支持的自动控制能力

当前 V1 已支持 3 层固定顺序的控制链：

1. 防逆流控制 `AntiReverseFlow`
2. SOC 上下限保护 `SocProtection`
3. 光伏限功率补偿 `PV Curtailment Compensation`

当前设计仍然是：

- 单站
- 单储能主控
- 单光伏补偿

也就是说，当前策略的主要可控对象仍然是储能，光伏限发是在储能无法继续消纳时接管的补偿动作。

## 3. 控制顺序

当前控制顺序固定，不做通用仲裁框架：

```text
Step 1: AntiReverseFlow
  先根据并网点功率计算储能目标功率

Step 2: SocProtection
  再根据 SOC 上下限对储能目标功率做裁剪

Step 3: PV Curtailment Compensation
  如果储能因为高 SOC 无法继续充电，且并网点仍有逆流，
  则通过 pv-target-power-limit 对光伏做限功率补偿
```

可以把它理解成：

- 防逆流负责回答“理论上应该怎么调”
- SOC 保护负责回答“储能允不允许这样调”
- 光伏限发负责回答“储能调不动以后，谁来兜底”

## 4. 三层策略的行为

### 4.1 防逆流控制

目标：

- 尽量让 `grid-active-power` 不低于 `export_limit_kw`
- 默认 `export_limit_kw = 0`
- 即默认不允许向电网反送电

当前实现是基于储能当前功率点的闭环调节：

- 检测并网点功率偏差
- 算出新的储能目标功率
- 再经过功率限幅、死区和爬坡率限制

主要参数：

- `export_limit_kw`
- `max_charge_kw`
- `max_discharge_kw`
- `deadband_kw`
- `ramp_rate_kw_per_s`

### 4.2 SOC 保护

目标：

- `SOC <= soc_low` 时禁止继续放电
- `SOC >= soc_high` 时禁止继续充电

当前 V1 中，SOC 保护不单独争抢控制权，而是作为上层裁剪器处理防逆流算出的储能目标。

典型效果：

- 如果防逆流希望储能充电，但 `SOC >= soc_high`
- 则储能目标被直接裁成 `0`
- 并记录抑制原因，例如：
  - `SOC(90%) >= high limit(80%), charge suppressed`

主要参数：

- `soc_low`
- `soc_high`

### 4.3 光伏限功率补偿

目标：

- 当并网点仍有逆流
- 且储能因为高 SOC 或不可用而无法继续接管
- 则自动通过 `pv-target-power-limit` 限制光伏出力

当前 V1 固定使用：

- `pv-target-power-limit`

不使用：

- `pv-target-power`

触发条件：

- `enable_pv_curtailment = true`
- `grid-active-power < export_limit_kw`
- 储能目标因高 SOC 被裁成 `0`
  或者储能当前不可用
- PV 运行状态有效
- 已配置 PV 功率点和 PV 限发设定点

计算逻辑：

1. 先计算当前超出的逆流量
2. 再从当前 PV 出力中扣掉这部分需要削减的功率
3. 用 `pv_rated_power_kw` 把目标有功功率换算成限发百分比
4. 再钳制到 `pv_limit_min_pct ~ pv_limit_max_pct`

恢复逻辑：

- 当逆流消失，且储能/SOC 约束解除后
- `pv-target-power-limit` 不会瞬间回到 `100%`
- 而是按 `pv_limit_recovery_step_pct` 逐步恢复

主要参数：

- `enable_pv_curtailment`
- `pv_rated_power_kw`
- `pv_limit_min_pct`
- `pv_limit_max_pct`
- `pv_limit_recovery_step_pct`

## 5. 当前推荐绑定角色

策略绑定继续保存在 `strategy_bindings` 表里，当前常用角色包括：

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

策略引擎会把同一基础角色下的多个设备聚合为站级控制对象：

- BESS 总目标先按站级计算，再按可运行、SOC 合法的设备平均分配
- PV 限发按统一百分比下发到所有参与该策略的 PV 设备

其中当前三层链路最重要的绑定一般是：

- `grid_power -> grid-active-power`
- `bess_power -> bess-active-power`
- `bess_soc -> bess-soc`
- `bess_run_state -> bess-run-mode`
- `bess_power_setpoint -> bess-target-power`
- `pv_power -> pv-active-power`
- `pv_power_limit_setpoint -> pv-target-power-limit`
- `pv_run_state -> pv-running-status`

## 6. 数据库存储

当前策略引擎主要使用以下 PostgreSQL 表：

- `strategy_definitions`
- `strategy_bindings`
- `strategy_params`
- `strategy_runtime_state`
- `strategy_action_logs`

用途分别是：

- `strategy_definitions`
  - 策略主表
- `strategy_bindings`
  - 输入点/输出点绑定
- `strategy_params`
  - 策略参数
- `strategy_runtime_state`
  - 当前运行状态、当前目标值、人工接管截止时间
- `strategy_action_logs`
  - 每次动作、抑制和恢复的明细日志

## 7. 人工接管规则

当前系统保留“人工优先”规则。

当用户通过 Web 手动下发以下点位时：

- `bess-target-power`
- `pv-target-power-limit`
- 以及其他绑定在策略上的手动控制点

系统会把关联策略写入人工接管窗口。

当前默认规则：

- 接管时长：30 分钟
- 接管期间：策略不再自动覆盖人工命令
- 到期后：策略自动恢复接管

## 8. 当前验证通过的典型场景

### 场景一：防逆流，SOC 正常

条件：

- `SOC = 50%`
- `PV = 36kW`
- `Load = 10kW`

预期：

- 储能进入充电
- `bess-target-power` 约为 `-26kW`
- `grid-active-power` 收敛到 `0kW` 附近

### 场景二：高 SOC，储能禁止继续充电，PV 接管限发

条件：

- `SOC = 90%`
- `PV = 36kW`
- `Load = 10kW`

预期：

- `bess-target-power = 0`
- `bess-active-power = 0`
- `pv-target-power-limit < 100`
- `grid-active-power` 收敛到 `0kW` 附近

## 9. 当前边界

当前暂不支持：

- 多储能协同
- 多光伏协同
- 需量控制
- 分时电价策略
- 光伏目标功率控制 `pv-target-power`
- 通用表达式引擎
- 脚本化策略

## 10. 后续建议

在当前三层链路之上，后续建议优先扩展：

1. 低 SOC 禁止放电的 E2E 场景
2. 手动接管 30 分钟 E2E 场景
3. BESS 停机时仅靠 PV 限发兜底
4. 光伏限发恢复的平滑释放策略
5. 多设备协同仲裁
