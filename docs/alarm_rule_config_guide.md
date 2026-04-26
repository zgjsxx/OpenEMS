# OpenEMS 告警规则配置说明

本文档说明当前 `openems-alarm` 如何通过 `alarm_rule.csv` 配置告警规则。

## 1. 配置文件位置

告警规则配置文件位于：

```text
config/tables/alarm_rule.csv
```

安装运行时对应位置为：

```text
install/config/tables/alarm_rule.csv
```

推荐从 `install/` 启动告警进程：

```powershell
.\install\start_alarm.ps1
```

该脚本会传入：

```text
config/tables
```

作为告警规则配置目录。

## 2. 字段定义

当前首版支持基础比较规则，字段固定为：

```csv
id,point_id,enabled,operator,threshold,severity,message
```

字段说明：

- `id`：告警规则 ID，必须唯一。
- `point_id`：关联点位 ID，必须存在于遥测、遥信、遥控或遥调点表中。
- `enabled`：是否启用，支持 `true` / `false`。
- `operator`：比较符，支持 `<`、`<=`、`>`、`>=`、`==`、`!=`。
- `threshold`：阈值，必须是数字。
- `severity`：告警等级，支持 `info`、`warning`、`critical`。
- `message`：告警消息。

本版暂不支持：

- 延时触发 `delay_ms`
- 回差 `hysteresis`
- 多点组合表达式
- 告警分组

## 3. 当前示例规则

当前样例配置如下：

```csv
id,point_id,enabled,operator,threshold,severity,message
bess-soc-low,bess-soc,true,<,10,critical,SOC too low
bess-soc-high,bess-soc,true,>,95,warning,SOC too high
bess-off-grid,bess-grid-state,true,==,1,warning,BESS is off-grid
pv-stopped,pv-running-status,true,==,0,warning,PV is stopped
pv-fault,pv-running-status,true,==,3,critical,PV fault
```

这几条规则等价于之前写在 `alarm_main.cpp` 里的硬编码逻辑。

## 4. 告警进程行为

`openems-alarm` 启动参数为：

```text
openems-alarm [shm_name] [output_path] [config_path]
```

默认值为：

```text
shm_name=Local\openems_rt_db
output_path=runtime/alarms_active.json
config_path=config/tables
```

启动后会读取：

```text
config_path/alarm_rule.csv
```

然后每 2 秒从 RtDb 读取点位值并判断规则。

读取点位时采用以下策略：

- 先尝试按遥测读取。
- 如果遥测不存在或无效，再尝试按遥信读取。
- 遥信状态码会按数字参与比较。

如果 `alarm_rule.csv` 缺失、为空或没有启用规则，告警进程不会崩溃，会输出空告警文件。

## 5. 输出文件

活动告警输出到：

```text
runtime/alarms_active.json
```

安装运行时对应位置为：

```text
install/runtime/alarms_active.json
```

Web 告警页会读取该文件，并同步到 PostgreSQL 的告警表中，用于确认、消音和历史查询。

## 6. Web 配置管理

`/comm` 通讯配置页面已经纳入 `alarm_rule.csv`。

当前支持：

- 查看告警规则。
- 新增、编辑、删除告警规则。
- 保存前校验。
- 保存前自动备份配置。
- 保存后提示需要重启 `openems-alarm`。

校验规则包括：

- `id` 必填且唯一。
- `point_id` 必须引用已有点位。
- `enabled` 必须为 `true` 或 `false`。
- `operator` 必须是支持的比较符。
- `threshold` 必须是数字。
- `severity` 必须是支持的告警等级。
- `message` 必填。

## 7. 验证步骤

推荐按下面流程验证：

1. 安装最新程序：

```powershell
cmake --build build-codex-vs2019 --config Release
cmake --install build-codex-vs2019 --config Release --prefix install
```

2. 启动采集程序：

```powershell
.\install\start_modbus_collector.ps1
```

3. 启动告警程序：

```powershell
.\install\start_alarm.ps1
```

4. 修改模拟器点位值：

```text
pv-running-status = 0
```

应触发：

```text
pv-stopped
```

5. 再修改为：

```text
pv-running-status = 3
```

应触发：

```text
pv-fault
```

6. 查看活动告警文件：

```powershell
Get-Content .\install\runtime\alarms_active.json
```

7. 打开 Web 告警页：

```text
http://127.0.0.1:8080/alarms
```

## 8. 后续扩展建议

后续可以在当前字段基础上继续扩展：

- `delay_ms`：满足条件持续一段时间后才触发。
- `hysteresis`：恢复时增加回差，避免临界值抖动。
- `group`：告警分组。
- `recover_message`：恢复消息。
- `enabled_schedule`：按时间段启用规则。
