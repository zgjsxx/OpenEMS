# OpenEMS 策略引擎 E2E 测试说明

本文档说明如何验证当前 `openems-strategy-engine` 的第一条端到端测试链路。

当前 E2E 测试聚焦两个策略能力：

- 防逆流控制
- SOC 上限保护

测试对象当前只针对储能设备 `bess-001`。

## 1. 测试目标

这条 E2E 链路验证的是完整系统行为，而不是单个函数：

1. 模拟器对外提供 Modbus TCP 数据
2. OpenEMS 采集模拟器数据并写入 RtDb
3. 策略引擎从 RtDb 读取实时点位
4. 策略引擎计算目标功率并写入命令槽
5. 控制链路把目标功率下发回模拟器
6. 模拟器状态回写，最终在 Web 和数据库里看到正确结果

## 2. 当前测试内容

### 2.1 场景一：防逆流控制

输入条件：

- `bess-soc = 50%`
- `pv-active-power = 36 kW`
- `load-power = 10 kW`
- `bess-active-power = 0 kW`
- `bess-target-power = 0 kW`

物理含义：

- 光伏发电 36 kW
- 站内负荷 10 kW
- 多余 26 kW 会从并网点反送到电网

预期结果：

- 策略引擎识别到反送电
- 自动把储能设为充电吸收功率
- `bess-target-power` 变为约 `-26 kW`
- `grid-active-power` 被拉回到约 `0 kW`

### 2.2 场景二：SOC 上限保护

输入条件：

- `bess-soc = 90%`
- `pv-active-power = 36 kW`
- `load-power = 10 kW`
- 其余条件与场景一相同

物理含义：

- 系统仍然存在 26 kW 的反送电趋势
- 但储能 SOC 已超过上限，不允许继续充电

预期结果：

- 防逆流策略仍然会算出一个充电目标
- SOC 保护策略会把该目标裁剪为 `0`
- `bess-target-power = 0 kW`
- `bess-active-power = 0 kW`
- `grid-active-power = -26 kW`
- 数据库中的策略运行状态应显示：
  - `suppressed = true`
  - `suppress_reason` 包含 `charge suppressed`

## 3. 测试环境组成

这条 E2E 链路由三部分组成：

- `openems-postgres`
  - PostgreSQL / TimescaleDB
- `openems-bess-sim`
  - 储能 / PV / 电网一体化模拟器
- `openems-app`
  - OpenEMS 主容器，内部启动：
  - `openems-rtdb-service`
  - `openems-modbus-collector`
  - `openems-history`
  - `openems-alarm`
  - `openems-strategy-engine`
  - `openems-admin-portal`

相关文件：

- `docker-compose.yml`
- `docker-compose.e2e.yml`
- `docker/e2e/simulator/simulator.py`
- `docker/e2e/config/tables/*.csv`
- `docker/e2e/seed_strategy_demo.sql`
- `scripts/run_strategy_e2e.py`

## 4. 前置条件

需要本机满足以下条件：

- 已安装 Docker Desktop
- 已启用 `docker compose`
- 本机 `8080` 端口可用
- 本机 `18080` 端口可用
- 本机可以正常拉取 Docker 基础镜像

建议先确认：

```powershell
docker version
docker compose version
```

## 5. 启动测试环境

在仓库根目录执行：

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml up --build -d
```

启动成功后，可检查容器状态：

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml ps
```

预期至少有这三个容器：

- `openems-postgres`
- `openems-bess-sim`
- `openems-app`

## 6. 运行自动化 E2E 测试

执行：

```powershell
python scripts\run_strategy_e2e.py
```

预期输出类似：

```text
[strategy-e2e] Logging into admin portal...
[strategy-e2e] Resetting simulator state...
[strategy-e2e] Running anti-reverse-flow scenario at SOC=50%...
[strategy-e2e] Anti-reverse-flow OK: bess-target-power=-26.00 kW, grid-active-power=0.00 kW
[strategy-e2e] Running SOC high clamp scenario at SOC=90%...
[strategy-e2e] SOC clamp OK: bess-target-power=0.00 kW, grid-active-power=-26.00 kW
[strategy-e2e] E2E validation passed.
```

只要最后看到：

```text
E2E validation passed.
```

就说明当前第一条策略链路通过。

## 7. 手动测试方法

除了脚本自动判断，建议再人工确认一次。

### 7.1 打开模拟器页面

浏览器打开：

```text
http://127.0.0.1:18080/
```

这个页面支持三种人工联调方式：

- 直接查看模拟器当前状态
- 手工修改表单并点击“应用表单到模拟器”
- 导入 CSV 场景表，并逐行点击“应用”

页面上还提供了“下载示例 CSV”入口，可直接拿来改。

CSV 推荐字段包括：

- `scenario_name`
- `note`
- `bess_soc_pct`
- `bess_active_power_w`
- `bess_target_power_w`
- `pv_power_w`
- `load_power_w`
- `bess_started`
- `bess_run_mode`
- `bess_grid_state`

其中：

- `scenario_name` 和 `note` 仅用于页面展示
- 其余字段会在点击“应用”时写入模拟器

### 7.2 用模拟器页面做防逆流测试

在页面中填入或导入这样一行：

```text
bess_soc_pct=50
bess_active_power_w=0
bess_target_power_w=0
pv_power_w=36000
load_power_w=10000
bess_started=true
bess_run_mode=1
bess_grid_state=0
```

点击“应用”后，回到 OpenEMS 后台查看：

- `bess-target-power`
- `bess-active-power`
- `grid-active-power`

预期：

- `bess-target-power` 变成约 `-26 kW`
- `bess-active-power` 逐步趋近 `-26 kW`
- `grid-active-power` 逐步趋近 `0 kW`

### 7.3 用模拟器页面做 SOC 高位抑制测试

在页面中填入或导入这样一行：

```text
bess_soc_pct=90
bess_active_power_w=0
bess_target_power_w=0
pv_power_w=36000
load_power_w=10000
bess_started=true
bess_run_mode=1
bess_grid_state=0
```

点击“应用”后，回到 OpenEMS 后台查看：

- `bess-target-power`
- `bess-active-power`
- `grid-active-power`

预期：

- `bess-target-power = 0 kW`
- `bess-active-power = 0 kW`
- `grid-active-power = -26 kW`

说明当前存在反送电趋势，但储能因高 SOC 被禁止继续充电。

### 7.4 查看 Web 实时点位

浏览器打开：

```text
http://127.0.0.1:8080/login
```

默认账号：

- 用户名：`admin`
- 密码：`admin123`

重点查看这些点：

- `bess-soc`
- `bess-active-power`
- `bess-target-power`
- `grid-active-power`

### 7.5 查看策略运行状态

可以直接查询数据库：

```powershell
docker exec openems-postgres psql -U postgres -d openems_admin -c "select strategy_id,current_target_value,suppressed,suppress_reason,updated_at from strategy_runtime_state order by strategy_id;"
```

在高 SOC 场景下，预期类似：

```text
e2e-anti-reverse-flow | 0 | t | SOC(90%) >= high limit(80%), charge suppressed
e2e-soc-protection    | 0 | t | SOC(90%) >= high limit(80%), charge suppressed
```

### 7.6 查看策略动作日志

执行：

```powershell
docker exec openems-postgres psql -U postgres -d openems_admin -c "select strategy_id,action_type,desired_value,suppress_reason,result,created_at from strategy_action_logs order by id desc limit 10;"
```

预期在高 SOC 场景下能看到：

- `desired_value = 0`
- `result = ok`
- `suppress_reason` 包含 `charge suppressed`

## 8. 测试脚本实际做了什么

`scripts/run_strategy_e2e.py` 依次做了以下事情：

1. 等待模拟器 HTTP 服务就绪
2. 等待后台登录页就绪
3. 登录后台
4. 重置模拟器状态
5. 等待 `grid-active-power` 点位变为有效
6. 执行场景一：
   - 把模拟器改为 `SOC=50%`
   - 等待 `bess-target-power < -5 kW`
   - 同时等待 `grid-active-power` 接近 `0`
7. 执行场景二：
   - 把模拟器改为 `SOC=90%`
   - 等待 `bess-target-power` 被压到 `0`
   - 同时等待策略状态出现 `charge suppressed`

## 9. 常用排障命令

### 9.1 查看 OpenEMS 容器日志

```powershell
docker logs openems-app --tail 200
```

### 9.2 查看模拟器当前状态

```powershell
docker exec openems-bess-sim python3 -c "import urllib.request, json; print(json.loads(urllib.request.urlopen('http://127.0.0.1:18080/state').read().decode()))"
```

### 9.3 查看策略进程是否运行

```powershell
docker exec openems-app pgrep -af openems-strategy-engine
```

### 9.4 重新拉起测试环境

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml down
docker compose -f docker-compose.yml -f docker-compose.e2e.yml up --build -d
```

如果连数据库卷也要一起清空：

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml down -v
```

## 10. 当前测试覆盖范围

当前这条 E2E 只覆盖：

- 单站
- 单储能设备
- Modbus TCP 模拟器
- 防逆流
- SOC 上限保护

还没有覆盖：

- SOC 下限禁止放电
- 手动接管 30 分钟
- 多储能协同
- 并网点限值动态变化
- IEC104 设备接入
- 真实现场设备

## 11. 建议的下一批测试

后续建议继续补这几条：

1. `SOC < 下限` 时禁止放电
2. 人工遥调后策略暂停接管
3. 反送电消失后储能目标逐步回到 `0`
4. 储能停机时策略抑制
5. 命令槽忙时策略不重复覆盖

这样这套 E2E 环境就可以逐步演进成策略模块的标准回归基线。
