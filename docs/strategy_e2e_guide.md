# OpenEMS 策略 E2E 测试说明

本文档说明当前策略链路的自动化测试和手工测试方法。

当前 E2E 主要覆盖 3 个能力的联动：

- 防逆流控制
- SOC 上限保护
- 高 SOC 时的光伏限功率补偿

## 1. 测试目标

这条 E2E 链路验证的是完整系统行为，而不是单个函数：

1. 模拟器提供 Modbus TCP 数据
2. OpenEMS 采集模拟器数据并写入 RtDb
3. 策略引擎从 RtDb 读取点位
4. 策略引擎下发储能或光伏控制目标
5. 控制链路把命令写回模拟器
6. 模拟器更新实际状态
7. Web 和 PostgreSQL 能看到正确结果

## 2. 当前测试内容

### 2.1 场景一：防逆流控制

输入条件：

- `bess_soc_pct = 50`
- `pv_available_power_w = 36000`
- `load_power_w = 10000`
- `bess_active_power_w = 0`

预期：

- `bess-target-power < 0`
- `bess-active-power` 逐步接近目标值
- `grid-active-power` 收敛到 `0kW` 附近

### 2.2 场景二：高 SOC + 光伏限功率补偿

输入条件：

- `bess_soc_pct = 90`
- `pv_available_power_w = 36000`
- `load_power_w = 10000`
- `bess_active_power_w = 0`

预期：

- `bess-target-power = 0`
- `bess-active-power = 0`
- `pv-target-power-limit < 100`
- `grid-active-power` 收敛到 `0kW` 附近

数据库预期：

- `strategy_runtime_state` 中
  - `e2e-soc-protection` 显示 `charge suppressed`
  - `e2e-anti-reverse-flow` 的 `current_target_point_id = pv-target-power-limit`
- `strategy_action_logs` 中能看到：
  - `strategy_id = e2e-anti-reverse-flow`
  - `action_type = pv_curtailment`
  - `target_point_id = pv-target-power-limit`

## 3. 测试环境组成

当前 E2E 环境由以下部分组成：

- `openems-postgres`
  - PostgreSQL / TimescaleDB
- `openems-bess-sim`
  - PV + BESS + Grid 一体化模拟器
- `openems-app`
  - OpenEMS 主容器

相关文件：

- `docker-compose.yml`
- `docker-compose.e2e.yml`
- `docker/e2e/simulator/simulator.py`
- `docker/e2e/seed_strategy_demo.sql`
- `scripts/run_strategy_e2e.py`

## 4. 启动环境

在仓库根目录执行：

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml up --build -d
```

检查容器状态：

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml ps
```

预期至少看到：

- `openems-postgres`
- `openems-bess-sim`
- `openems-app`

## 5. 运行自动化 E2E

执行：

```powershell
python scripts\run_strategy_e2e.py
```

预期输出类似：

```text
[strategy-e2e] Running anti-reverse-flow scenario at SOC=50%...
[strategy-e2e] Anti-reverse-flow OK: bess-target-power=-26.00 kW, grid-active-power=0.00 kW
[strategy-e2e] Running SOC high clamp with PV curtailment scenario at SOC=90%...
[strategy-e2e] SOC clamp + PV curtailment OK: bess-target-power=0.00 kW, bess-active-power=0.00 kW, pv-target-power-limit=10.00 %, grid-active-power=0.00 kW
[strategy-e2e] E2E validation passed.
```

只要最后看到：

```text
E2E validation passed.
```

就说明当前三层策略链路通过。

## 6. 手工测试

### 6.1 打开模拟器页面

浏览器打开：

```text
http://127.0.0.1:18080/
```

这个页面支持：

- 查看当前模拟器状态
- 手工修改状态并应用
- 导入 CSV 场景表
- 逐行应用 CSV 场景
- 下载示例 CSV

### 6.2 推荐关注的模拟器字段

手工测试时，最关键的是以下几个字段：

- `pv_available_power_w`
  - 光伏当前可发功率，相当于“太阳给了多少”
- `pv_power_w`
  - 光伏实际输出功率，会受到 `pv-target-power-limit` 影响
- `bess_soc_pct`
- `bess_active_power_w`
- `bess_target_power_w`
- `load_power_w`

注意：

- 现在建议把 `pv_available_power_w` 作为场景输入
- `pv_power_w` 更多用于观察实际输出结果
- 为了兼容旧场景，页面和接口仍接受 `pv_power_w` 作为输入别名

### 6.3 手工测试场景一：防逆流

在模拟器页面中设置：

```text
bess_soc_pct = 50
bess_active_power_w = 0
bess_target_power_w = 0
pv_available_power_w = 36000
load_power_w = 10000
bess_started = true
bess_run_mode = 1
bess_grid_state = 0
```

点击“应用”后，回到 OpenEMS 后台观察：

- `bess-target-power`
- `bess-active-power`
- `grid-active-power`

预期：

- `bess-target-power` 变成约 `-26kW`
- `bess-active-power` 逐步接近 `-26kW`
- `grid-active-power` 逐步接近 `0kW`

### 6.4 手工测试场景二：高 SOC + 光伏限发

在模拟器页面中设置：

```text
bess_soc_pct = 90
bess_active_power_w = 0
bess_target_power_w = 0
pv_available_power_w = 36000
load_power_w = 10000
bess_started = true
bess_run_mode = 1
bess_grid_state = 0
```

点击“应用”后，回到 OpenEMS 后台观察：

- `bess-target-power`
- `bess-active-power`
- `pv-target-power-limit`
- `pv-active-power`
- `grid-active-power`

预期：

- `bess-target-power = 0`
- `bess-active-power = 0`
- `pv-target-power-limit < 100`
- `pv-active-power` 降低到接近负荷水平
- `grid-active-power` 收敛到 `0kW` 附近

### 6.5 用 CSV 做手工回放

模拟器页面支持导入 CSV 场景表。

当前推荐字段：

- `scenario_name`
- `note`
- `bess_soc_pct`
- `bess_active_power_w`
- `bess_target_power_w`
- `pv_available_power_w`
- `load_power_w`
- `bess_started`
- `bess_run_mode`
- `bess_grid_state`

可以先下载页面内置的示例 CSV，再按自己的测试场景修改。

### 6.6 手工查看后台页面

OpenEMS 后台地址：

```text
http://127.0.0.1:8080/login
```

默认账号：

- 用户名：`admin`
- 密码：`admin123`

建议同时打开：

- 实时看板
- 策略管理
- 系统监控

## 7. 查看数据库状态

查看策略运行状态：

```powershell
docker exec openems-postgres psql -U postgres -d openems_admin -c "select strategy_id,current_target_value,current_target_point_id,suppressed,suppress_reason,updated_at from strategy_runtime_state order by strategy_id;"
```

查看策略动作日志：

```powershell
docker exec openems-postgres psql -U postgres -d openems_admin -c "select strategy_id,action_type,target_point_id,desired_value,suppress_reason,result,created_at from strategy_action_logs order by id desc limit 20;"
```

## 8. 常用排障命令

查看 OpenEMS 容器日志：

```powershell
docker logs openems-app --tail 200
```

查看模拟器状态：

```powershell
Invoke-RestMethod http://127.0.0.1:18080/state
```

查看策略进程：

```powershell
docker exec openems-app pgrep -af openems-strategy-engine
```

重新拉起环境：

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml down
docker compose -f docker-compose.yml -f docker-compose.e2e.yml up --build -d
```

如果连数据库卷也一起清空：

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml down -v
```

## 9. 当前覆盖范围

当前 E2E 主要覆盖：

- 单站
- 单储能
- 单光伏
- Modbus TCP 模拟器
- 防逆流
- SOC 高位保护
- 光伏限功率补偿

尚未覆盖：

- 低 SOC 禁止放电
- 手动接管 30 分钟
- 储能停机后仅靠 PV 限发兜底
- 光伏限发的逐步恢复验证
- 多设备协同

## 10. 下一步建议补测

建议下一批补这几条：

1. `SOC < 下限` 时禁止放电
2. 手动下发 `bess-target-power` 后策略暂停接管
3. 手动下发 `pv-target-power-limit` 后策略暂停接管
4. 逆流消失后 `pv-target-power-limit` 逐步恢复到 `100`
5. `bess-run-mode = 0` 时由 PV 限发单独兜底
