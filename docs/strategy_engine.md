# OpenEMS Strategy Engine V1 设计文档

## 架构概述

策略引擎作为独立进程 `openems-strategy-engine` 运行，与现有采集/监控/告警/历史服务同级。采用"单进程、单线程、固定周期循环"模式，只读 RtDb 实时点位，只通过现有命令槽下发 telecontrol/teleadjust。

```
                     PostgreSQL
                  (策略配置 + 运行状态 + 审计日志)
                        |
                openems-strategy-engine
                        |
              ====== RtDb (共享内存) ======
              |           |              |
        [rtdb-service] [collector] [web console]
```

## 控制仲裁链路

V1 的控制仲裁固定为两层级联：

```
Step 1: AntiReverseFlow (防逆流)
  输入: P_grid (并网点有功功率), BESS run_state
  输出: raw_target_kw (负值=充电消纳, 正值=放电, 0=无动作)

Step 2: SocProtection (SOC 保护, 裁剪器)
  输入: SOC (%), raw_target_kw
  输出: clamped_target_kw (对 raw_target 做限幅)
```

最终只产生一个设备目标值并下发到 BESS 功率设定点。

## 防逆流控制算法

### 目标

并网点有功功率 P_grid 不超过配置阈值 export_limit_kw（默认 0），即不向电网反送电。

### 算法（比例前馈控制）

```
if P_grid < export_limit_kw:
    // 反送电检测：P_grid 低于出口限制
    raw_target = P_grid - export_limit_kw  // 负值: 充电消纳
else:
    raw_target = 0  // 无反送电, 释放储能

// 功率限幅
raw_target = clamp(raw_target, -max_charge_kw, max_discharge_kw)

// 爬坡率限制
max_step = ramp_rate_kw_per_s * dt
target = clamp(raw_target, prev_target - max_step, prev_target + max_step)

// 死区
if |target| < deadband_kw: target = 0

// 去抖：变化量 < deadband/2 不重复下发
// 命令槽忙 (Pending/Executing) 不覆盖
```

### 边界场景

| 场景 | P_grid | 行为 |
|---|---|---|
| 无反送电 | P_grid ≥ 0 | target = 0, 释放储能 |
| 轻度反送电 | P_grid = -10kW | target = -10kW, 充电消纳 |
| 反送电超充电能力 | P_grid = -100kW, max_charge=50kW | target = -50kW (饱和), 记录"储能无法消纳" |
| BESS 不可用 | run_state=0 | 抑制, 不下发命令 |

## SOC 上下限保护算法

### 目标

- SOC ≤ soc_low: 禁止继续放电, 只允许充电（clamp target ≤ 0）
- SOC ≥ soc_high: 禁止继续充电, 只允许放电（clamp target ≥ 0）

### 算法（限幅裁剪器）

```
if SOC <= soc_low and target > 0:
    // 正在放电, 禁止
    clamped = 0, suppressed = true
    reason = "SOC(%xx%) <= low limit(%yy%), discharge suppressed"

elif SOC >= soc_high and target < 0:
    // 正在充电, 禁止
    clamped = 0, suppressed = true
    reason = "SOC(%xx%) >= high limit(%yy%), charge suppressed"

else:
    // SOC 在正常范围内, 放行
    clamped = target, suppressed = false
```

### 边界场景：SOC 超上限 + 反送电同时发生

| 条件 | 处理 |
|---|---|
| SOC ≥ soc_high, P_grid < 0 | 防逆流要充电(target<0), SOC 禁止充电 → clamp 到 0 |
| 结果 | 储能 idle, 反送电无法消纳, 记录状态供运维参考 |

**原因**: 储能已满, 无法继续吸收功率。反送电问题需通过弃光/限功率等非储能手段解决。

## 仲裁顺序说明

防逆流先算、SOC 后裁剪的顺序是物理必然的：

1. 防逆流回答"需要多少功率来消除反送电"（可能是充电或放电）
2. SOC 保护回答"在当前 SOC 下, 这个方向的功率流动是否允许"

两者取交集：防逆流的需求 ∩ SOC 的许可 = 最终执行值。当交集为空时（如上述 SOC 满 + 反送电场景），储能无法动作，需记录状态。

## 人工接管机制

### 触发条件

用户通过 Web 遥控/遥调接口（POST /api/command）对某设备下发手动命令时：

1. 命令正常写入 RtDb CommandSlot
2. 同时查询该点位所属 device_id
3. 查找所有 target 该 device 的 enabled 策略
4. 写入 strategy_runtime_state.manual_override_until = NOW() + 30 分钟

### 接管期间行为

策略引擎主循环每周期检查 manual_override_until:
- 若 override_until > now(): 跳过该策略, 不读写 command slot
- 到期后自动恢复接管

### 设计决策

- **粒度**: 设备级。对策略输出点所在设备的任何手动操作都触发接管。
- **时长**: V1 固定 30 分钟, 不做每设备自定义。
- **优先级**: 人工 > 自动, 不可配置。

## 数据库表结构

### strategy_definitions
策略定义主表。字段: id, name, type(anti_reverse_flow|soc_protection), enabled, site_id, device_id, priority(越小越优先), cycle_ms, created_at, updated_at。

### strategy_bindings
点位绑定表。字段: id, strategy_id, role(grid_power|bess_power|bess_soc|bess_run_state|bess_power_setpoint), point_id。

### strategy_params
策略参数表。字段: id, strategy_id, param_key, param_value(文本存储, 由策略引擎解析)。

防逆流参数: export_limit_kw, max_charge_kw, max_discharge_kw, deadband_kw, ramp_rate_kw_per_s。
SOC 参数: soc_low, soc_high。
通用参数: manual_override_minutes。

### strategy_runtime_state
运行状态表（UPSERT）。字段: strategy_id, last_execution_time, current_target_value, current_target_point_id, suppressed, suppress_reason, manual_override_until, last_error, input_summary(JSONB), updated_at。

### strategy_action_logs
动作日志表（INSERT only）。字段: id(BIGSERIAL), strategy_id, action_type(command|suppress|error), target_point_id, desired_value, result_value, suppress_reason, input_summary(JSONB), result(ok|suppressed|failed), details, created_at。

## 部署

### 环境变量
- `OPENEMS_DB_URL`: PostgreSQL 连接串（必填）
- `OPENEMS_ENABLE_STRATEGY=1`: Docker 容器中启用策略引擎

### 启动命令
```bash
# Docker
OPENEMS_ENABLE_STRATEGY=1 docker-compose up

# 独立进程
./install/bin/openems-strategy-engine [shm_name]

# 脚本
./scripts/start_strategy_engine.sh
```

## 未覆盖的 V2+ 规划

- 多设备协同（多台储能/PV 协调）
- 多策略复杂仲裁（优先级、权重、条件组合）
- 通用表达式引擎 / 脚本化策略
- 光伏限功率控制
- PCS 模式切换控制
- 需量控制
- 每设备可配置的手动接管时长
- 前馈 + 积分控制的防逆流增强（消除稳态误差）
