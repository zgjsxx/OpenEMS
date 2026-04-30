# OpenEMS 策略 E2E 与手动联调说明

本文档说明当前策略链路的自动化测试和手动测试方法。  
当前测试环境已经扩成多设备场景：

- 2 台光伏：`pv-001`、`pv-002`
- 2 台储能：`bess-001`、`bess-002`
- 1 个关口表 / 并网点：`grid-001`

当前覆盖的策略能力：

- 防逆流控制
- SOC 上限保护
- 高 SOC 时的光伏限功率补偿

## 1. 测试目标

这条 E2E 链路验证的是完整系统行为，而不是单个函数：

1. 模拟器提供多设备 Modbus TCP 数据
2. OpenEMS 采集两台 PV、两台 BESS 和关口表数据并写入 RtDb
3. 策略引擎从 RtDb 读取站级点位
4. 策略引擎对多台储能分摊目标功率，必要时对多台光伏统一限发
5. 控制链路把命令写回模拟器
6. 模拟器更新实际出力、SOC、关口表功率
7. Web 和 PostgreSQL 能看到正确结果

## 2. 当前测试内容

### 2.1 场景一：双储能防逆流

输入条件：

- `pv-001 = 18kW`
- `pv-002 = 18kW`
- `load = 10kW`
- `bess-001 SOC = 50%`
- `bess-002 SOC = 55%`
- 两台储能都可运行

预期：

- 总储能目标功率约为 `-26kW`
- 两台储能各承担约一半目标功率
- `grid-active-power` 收敛到 `0kW` 附近

### 2.2 场景二：双储能高 SOC + 双光伏限发

输入条件：

- `pv-001 = 18kW`
- `pv-002 = 18kW`
- `load = 10kW`
- `bess-001 SOC = 90%`
- `bess-002 SOC = 92%`
- 两台储能都可运行，但都因高 SOC 不能继续充电

预期：

- `bess-001 target = 0`
- `bess-002 target = 0`
- `pv-001 target power limit < 100%`
- `pv-002 target power limit < 100%`
- `grid-active-power` 收敛到 `0kW` 附近

数据库预期：

- `strategy_runtime_state` 中：
  - `e2e-soc-protection` 显示 `all BESS SOC >= high limit...`
  - `e2e-anti-reverse-flow` 的 `current_target_point_id` 包含 PV 限发点
- `strategy_action_logs` 中能看到：
  - `action_type = pv_curtailment`
  - `target_point_id = pv-target-power-limit,pv2-target-power-limit`

## 3. 测试环境组成

当前 E2E 环境由以下部分组成：

- `openems-postgres`
  - PostgreSQL / TimescaleDB
- `openems-bess-sim`
  - 双 PV + 双 BESS + Grid 一体化模拟器
- `openems-app`
  - OpenEMS 主容器

相关文件：

- [docker-compose.yml](/D:/git_proj/OpenEMS/docker-compose.yml)
- [docker-compose.e2e.yml](/D:/git_proj/OpenEMS/docker-compose.e2e.yml)
- [simulator.py](/D:/git_proj/OpenEMS/docker/e2e/simulator/simulator.py)
- [seed_strategy_demo.sql](/D:/git_proj/OpenEMS/docker/e2e/seed_strategy_demo.sql)
- [run_strategy_e2e.py](/D:/git_proj/OpenEMS/scripts/run_strategy_e2e.py)

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
[strategy-e2e] Running multi-device anti-reverse-flow scenario...
[strategy-e2e] Anti-reverse-flow OK: total-bess-target=-26.00 kW, grid-active-power=0.00 kW
[strategy-e2e] Running multi-device SOC high clamp with PV curtailment scenario...
[strategy-e2e] SOC clamp + PV curtailment OK: total-bess-target=0.00 kW, total-bess-active=0.00 kW, pv1-limit=5.00 %, pv2-limit=5.00 %, grid-active-power=0.00 kW
[strategy-e2e] E2E validation passed.
```

只要最后看到：

```text
E2E validation passed.
```

就说明当前多设备策略链路通过。

## 6. 手动测试

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

### 6.2 关键模拟器字段

建议重点关注这些字段：

- 站级：
  - `load_power_w`
- 光伏：
  - `pv_available_power_w`
  - `pv2_available_power_w`
  - `pv_target_power_limit_pct`
  - `pv2_target_power_limit_pct`
- 储能：
  - `bess_soc_pct`
  - `bess2_soc_pct`
  - `bess_target_power_w`
  - `bess2_target_power_w`
  - `bess_started`
  - `bess2_started`
  - `bess_run_mode`
  - `bess2_run_mode`

说明：

- `pv_available_power_w` / `pv2_available_power_w` 表示当前光伏最大可发功率
- 模拟器会根据 `pv_target_power_limit_pct` 和 `pv2_target_power_limit_pct` 自动计算实际出力
- 两台储能的实际功率会按爬坡速率逐步逼近目标功率

### 6.3 手动测试场景一：双储能防逆流

在模拟器页面中设置：

```text
load_power_w = 10000
pv_available_power_w = 18000
pv2_available_power_w = 18000
bess_soc_pct = 50
bess2_soc_pct = 55
bess_target_power_w = 0
bess2_target_power_w = 0
bess_started = true
bess2_started = true
bess_run_mode = 1
bess2_run_mode = 1
```

点击“应用表单到模拟器”后，回到 OpenEMS 后台观察：

- `bess-target-power`
- `bess2-target-power`
- `bess-active-power`
- `bess2-active-power`
- `grid-active-power`

预期：

- 两台储能目标功率都变成负值
- 总目标约 `-26kW`
- 两台储能实际功率逐步逼近目标
- `grid-active-power` 逐步接近 `0kW`

### 6.4 手动测试场景二：高 SOC + 双光伏限发

在模拟器页面中设置：

```text
load_power_w = 10000
pv_available_power_w = 18000
pv2_available_power_w = 18000
bess_soc_pct = 90
bess2_soc_pct = 92
bess_target_power_w = 0
bess2_target_power_w = 0
bess_started = true
bess2_started = true
bess_run_mode = 1
bess2_run_mode = 1
```

点击“应用表单到模拟器”后，回到 OpenEMS 后台观察：

- `bess-target-power`
- `bess2-target-power`
- `bess-active-power`
- `bess2-active-power`
- `pv-target-power-limit`
- `pv2-target-power-limit`
- `pv-active-power`
- `pv2-active-power`
- `grid-active-power`

预期：

- 两台储能目标和实际都接近 `0`
- 两台光伏限发百分比都会下降到 `100%` 以下
- 两台光伏实际出力下降
- `grid-active-power` 最终回到 `0kW` 附近

### 6.5 用 CSV 做手工回放

模拟器页面支持导入 CSV 场景表。

当前推荐字段：

- `scenario_name`
- `note`
- `load_power_w`
- `pv_available_power_w`
- `pv2_available_power_w`
- `bess_soc_pct`
- `bess2_soc_pct`
- `bess_started`
- `bess2_started`
- `bess_run_mode`
- `bess2_run_mode`
- `bess_grid_state`
- `bess2_grid_state`

建议流程：

1. 先从模拟器页面下载示例 CSV
2. 按你的手工联调场景修改每一行
3. 导入页面
4. 对某一行点“应用”
5. 在 OpenEMS 后台观察策略行为

### 6.6 手动查看后台页面

OpenEMS 后台地址：

```text
http://127.0.0.1:8080/login
```

默认账号：

- 用户名：`admin`
- 密码：`admin123`

建议同时打开：

- 首页看板
- 策略管理
- 系统监控
- 拓扑管理

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

如果连数据库卷也要一起清空：

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml down -v
```

## 9. 当前覆盖范围

当前 E2E 主要覆盖：

- 单站
- 双储能
- 双光伏
- 单关口表
- Modbus TCP 模拟器
- 防逆流
- SOC 高位保护
- 光伏限功率补偿
- 多设备目标分配

尚未覆盖：

- 低 SOC 禁止放电
- 手动接管 30 分钟
- 储能停机后仅靠 PV 限发兜底
- 光伏限发逐步恢复的长期场景
- 不同功率等级设备按权重分配

## 10. 下一步建议补测

建议下一批补这些：

1. `SOC < 下限` 时禁止放电
2. 手动下发 `bess-target-power` 后策略暂停接管
3. 手动下发 `pv-target-power-limit` 后策略暂停接管
4. 逆流消失后 PV 限发逐步恢复到 `100%`
5. 一台 BESS 故障停机后，另一台 BESS 自动承担更多目标
6. 两台 BESS 容量不一致时的目标分配
