# OpenEMS 告警规则配置说明

本文档说明当前 `openems-alarm` 如何通过 PostgreSQL `alarm_rules` 表配置告警规则。

## 1. 配置存储

告警规则存储在 PostgreSQL 的 `alarm_rules` 结构化配置表中，运行时仅从 PostgreSQL 读取。

表结构：

```sql
CREATE TABLE alarm_rules (
    id VARCHAR(128) PRIMARY KEY,
    point_id VARCHAR(128) NOT NULL REFERENCES points(id) ON DELETE CASCADE,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    operator VARCHAR(8) NOT NULL,
    threshold DOUBLE PRECISION NOT NULL,
    severity VARCHAR(16) NOT NULL,
    message TEXT NOT NULL
);
```

字段说明：

- `id`：告警规则 ID，必须唯一。
- `point_id`：关联点位 ID，必须存在于 `points` 表中。
- `enabled`：是否启用，布尔值。
- `operator`：比较符，支持 `<`、`<=`、`>`、`>=`、`==`、`!=`。
- `threshold`：阈值，数字。
- `severity`：告警等级，支持 `info`、`warning`、`critical`。
- `message`：告警消息。

CSV 保留为导入、导出、备份和初始 bootstrap 格式。运行时不再直接读取 CSV。

## 2. 示例规则

默认种子数据（migration 004）中包含以下示例规则：

| id | point_id | enabled | operator | threshold | severity | message |
|---|---|---|---|---|---|---|
| bess-soc-low | bess-soc | true | < | 10 | critical | SOC too low |
| bess-soc-high | bess-soc | true | > | 95 | warning | SOC too high |
| bess-off-grid | bess-grid-state | true | == | 1 | warning | BESS is off-grid |
| pv-stopped | pv-running-status | true | == | 0 | warning | PV is stopped |
| pv-fault | pv-running-status | true | == | 3 | critical | PV fault |

如果结构化配置表为空，可以通过 CSV 导入：

```powershell
python src/web/sync_config_to_postgres.py --mode import --config-dir config/tables
```

## 3. 告警进程行为

### 3.1 启动参数

```text
openems-alarm [shm_name]
```

参数说明：

- `shm_name`：共享内存名称，默认使用平台内置值

### 3.2 运行要求

告警进程强依赖 PostgreSQL：

- 必须设置 `OPENEMS_DB_URL` 环境变量，缺则报错退出
- 必须有 `libpq` 运行库，缺则报错退出
- 必须能连接 PostgreSQL，连接失败则退出

### 3.3 规则加载与刷新

启动后从 PostgreSQL 加载所有 `enabled=true` 的告警规则。

每 30 秒自动重新读取规则，Web 页面修改后无需手动重启告警进程，修改会在下一个刷新周期生效。

### 3.4 告警判断周期

每 2 秒从 RtDb 读取点位值并判断规则。

读取点位时采用以下策略：

- 先尝试按遥测读取。
- 如果遥测不存在或无效，再尝试按遥信读取。
- 遥信状态码会按数字参与比较。

### 3.5 活动告警同步

告警进程将活动告警直接同步写入 PostgreSQL `alarm_events` 表，不再写入 `runtime/alarms_active.json` 文件。

## 4. Web 配置管理

`/comm` 通讯配置页面已纳入告警规则管理。

当前支持：

- 查看告警规则。
- 新增、编辑、删除告警规则。
- 保存前校验。
- 保存前自动备份配置。
- 保存后告警进程会在下一个刷新周期自动加载新规则，无需重启。

校验规则包括：

- `id` 必填且唯一。
- `point_id` 必须引用 `points` 表中已有点位。
- `enabled` 必须为布尔值。
- `operator` 必须是支持的比较符。
- `threshold` 必须是数字。
- `severity` 必须是支持的告警等级。
- `message` 必填。

## 5. Docker 环境下启动

Docker 容器中通过 `OPENEMS_ENABLE_ALARM=1` 控制告警进程是否启动：

```yaml
environment:
  OPENEMS_ENABLE_ALARM: "1"
```

详见 `docs/docker_deploy.md`。

## 6. 验证步骤

推荐按下面流程验证：

1. 确保 PostgreSQL 已启动并完成 migration 初始化。

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

应触发 `pv-stopped` 告警。

5. 再修改为：

```text
pv-running-status = 3
```

应触发 `pv-fault` 告警。

6. 打开 Web 告警页：

```text
http://127.0.0.1:8080/alarms
```

确认活动告警列表中有对应记录。

## 7. 后续扩展建议

后续可以在当前字段基础上继续扩展：

- `delay_ms`：满足条件持续一段时间后才触发。
- `hysteresis`：恢复时增加回差，避免临界值抖动。
- `group`：告警分组。
- `recover_message`：恢复消息。
- `enabled_schedule`：按时间段启用规则。