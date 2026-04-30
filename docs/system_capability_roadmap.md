# OpenEMS 当前功能与后续开发建议

本文档用于梳理 OpenEMS 当前已经具备的能力，以及后续建议补齐的功能边界。它更偏项目路线图，不替代具体模块的使用手册。

## 1. 当前系统定位

OpenEMS 当前已经具备一个单站能源管理系统的基础形态：

- C++ 进程负责采集、控制、实时数据、告警和历史采样。
- Python FastAPI 后台负责登录、用户、配置、告警、历史、审计、实时看板、拓扑展示、策略管理和系统监控。
- PostgreSQL 是运行时配置主源，CSV 保留为导入、导出、备份和初始 bootstrap 格式。
- PostgreSQL 同时承载后台管理数据（用户、会话、告警处理状态、审计日志）和运行时配置。
- `install/` 目录已经作为推荐运行根目录，用于统一 `bin/`、`config/`、`runtime/` 和 `web/`。

当前项目已经超过简单 demo 阶段，但仍处于“单站本地交付 + 运维后台成型”的阶段。后续重点应放在可交付性、可观测性、操作安全和现场配置体验上。

## 2. 当前已经支持的功能

### 2.1 运行与部署

当前已经支持：

- 基于 CMake 的 C++17 构建。
- Visual Studio 2019 Release 构建验证。
- `cmake --install` 安装到仓库根目录下的 `install/`。
- 统一安装目录结构：
  - `install/bin/`
  - `install/config/`
  - `install/runtime/`
  - `install/web/`
- 安装后启动脚本：
  - `start_modbus_collector.ps1`
  - `start_iec104_collector.ps1`
  - `start_history.ps1`
  - `start_alarm.ps1`
  - `start_admin_portal.ps1`
  - `init_admin_portal_db.ps1`
- 推荐从 `install/` 根目录运行，避免 `build/.../runtime` 与 Web 读取目录不一致。

### 2.2 配置体系

当前运行时配置主源为 PostgreSQL 结构化配置表，CSV 保留为导入、导出、备份和初始 bootstrap 格式：

- `site.csv` → `sites` 表
- `device.csv` → `devices` 表
- `ems_config.csv` → `ems_config` 表
- `telemetry.csv` → `points` 表（遥测）
- `teleindication.csv` → `points` 表（遥信）
- `telecontrol.csv` → `points` 表（遥控）
- `teleadjust.csv` → `points` 表（遥调）
- `modbus_mapping.csv` → `modbus_mappings` 表
- `iec104_mapping.csv` → `iec104_mappings` 表
- `alarm_rule.csv` → `alarm_rules` 表
- `topology_node.csv` → `topology_nodes` 表
- `topology_link.csv` → `topology_links` 表
- `topology_binding.csv` → `topology_bindings` 表

当前已经支持的配置能力包括：

- 站点、设备、点位、协议映射配置。
- 遥测、遥信、遥控、遥调四类点表。
- `device.csv` 整行 `#` 注释，用于调试时临时屏蔽设备（仅 CSV 编辑时有效）。
- Web `/comm` 页面在线查看、编辑、校验和保存 CSV。
- Web `/config` 页面直接操作 PostgreSQL 结构化配置表。
- 保存配置前自动备份到 `runtime/config_backups/`。
- 配置保存后需要重启的相关 C++ 进程会提示；告警进程每 30 秒自动刷新规则。

当前 Web 配置校验已经覆盖：

- 必填字段校验。
- 设备 ID 唯一性。
- 点位 ID 全局唯一性。
- 协议与映射关系校验。
- `writable`、`enabled` 字段合法性。
- `telecontrol` / `teleadjust` 必须 `writable=true`。
- Modbus 功能码、`data_type`、`register_count` 基本约束。
- IEC104 基础字段合法性。
- 同一 `point_id` 不能同时存在 Modbus 和 IEC104 映射。
- 告警规则、拓扑节点、拓扑连接、拓扑点位绑定的引用关系校验。

### 2.3 Modbus 采集与控制

当前已经支持：

- Modbus TCP 设备连接。
- 按设备轮询周期采集。
- 按点表和 `modbus_mapping.csv` 解析遥测、遥信。
- 遥控、遥调点配置。
- 遥控遥调命令下发。
- 写功能码支持 `5`、`6`、`16`。
- 多寄存器数值编码写入。
- 控制点可通过 Web 看板发起下发。
- 控制操作写入审计日志。

当前需要注意：

- 写点的回读、执行确认、失败原因和互锁策略仍需增强。
- 部分设备的寄存器地址、字节序、字序仍需要按厂家手册校准。

### 2.4 IEC104 采集

当前已经支持：

- IEC104 设备配置。
- IEC104 collector 可执行程序。
- 根据 `iec104_mapping.csv` 进行点位映射。
- IEC104 采集数据写入 RtDb。
- 仓库中包含 IEC104 测试服务工具。

当前 IEC104 能力仍偏采集侧，控制、总召、对时、遥控确认流程和规约细节测试还需要继续完善。

### 2.5 RtDb 实时数据库

当前已经支持：

- C++ 共享内存实时数据库。
- 采集进程创建 RtDb。
- Viewer、Alarm、History、Web 读取 RtDb。
- 遥测、遥信、命令槽等基础结构。
- Web 后台通过 `shm_reader.py` 读取实时快照和命令状态。

RtDb 是当前系统实时链路的核心。

### 2.6 历史采样

当前已经支持：

- `openems-history` 历史采样进程。
- 从 RtDb 读取实时点位。
- 按天写入 JSONL 历史文件。
- 默认输出到 `runtime/history/`。
- Web 历史页按点位、时间范围查询历史数据。

当前历史能力适合调试和轻量使用，还不是完整时序数据库方案。

### 2.7 告警

当前已经支持：

- `openems-alarm` 告警进程。
- 从 RtDb 读取点位值。
- 从 PostgreSQL `alarm_rules` 表读取告警规则（每 30 秒自动刷新）。
- 支持比较符 `<`、`<=`、`>`、`>=`、`==`、`!=`。
- 支持等级 `info`、`warning`、`critical`。
- 活动告警直接同步到 PostgreSQL `alarm_events` 表。
- Web 告警页从 PostgreSQL 读取活动告警。
- 告警确认、消音。
- 告警处理动作写入审计日志。

当前告警规则字段（PostgreSQL `alarm_rules` 表）：

```text
id, point_id, enabled, operator, threshold, severity, message
```

当前暂不支持：

- 延时触发。
- 回差。
- 复合表达式告警。
- 告警恢复条件单独配置。

### 2.8 拓扑管理

当前已经支持拓扑管理 V1：

- 新增 `/topology` 拓扑管理页面。
- 新增 `/api/topology` API。
- 使用原生 SVG/CSS 展示站点电气单线图。
- 拓扑配置通过 CSV 管理：
  - `topology_node.csv`
  - `topology_link.csv`
  - `topology_binding.csv`
- 拓扑节点支持 Grid、Bus、Breaker、PV、BESS、Load 等类型。
- 节点和线路可以绑定点位，用于展示功率、电压、电流、SOC、状态和告警。
- 拓扑页面会合并 CSV 配置、RtDb 实时快照和活动告警。
- 点击拓扑节点可以查看绑定点位、实时值、质量、时间和告警详情。
- 已内置一套 `site-demo-001` 测试拓扑数据，便于后续验证。

当前拓扑编辑方式仍是“表结构编辑”，适合验证数据模型和版本化配置。更符合现场运维习惯的方式应是后续增加拖拽式拓扑编辑器。

### 2.9 Web 单站运维后台

当前已经支持：

- 登录页面。
- 用户会话。
- PostgreSQL 后台数据存储。
- 角色权限：
  - `viewer`
  - `operator`
  - `admin`
- 首页看板。
- 告警管理。
- 历史查询。
- 配置概览。
- 配置管理。
- 拓扑管理。
- 用户管理。
- 审计日志。

当前主要页面包括：

- `/login`
- `/dashboard`
- `/alarms`
- `/history`
- `/config`
- `/comm`
- `/topology`
- `/strategy`
- `/system`
- `/users`
- `/audit`

当前主要 API 包括：

- `/api/auth/login`
- `/api/auth/logout`
- `/api/auth/me`
- `/api/system/status`
- `/api/snapshot`
- `/api/command`
- `/api/command/{point_id}`
- `/api/alarms`
- `/api/alarms/active`
- `/api/alarms/{alarm_id}/ack`
- `/api/alarms/{alarm_id}/silence`
- `/api/history/points`
- `/api/history/query`
- `/api/history/query_multi`
- `/api/history/query_aggregate`
- `/api/config/overview`
- `/api/config`
- `/api/config-editor/schema`
- `/api/config-editor/data`
- `/api/config-editor/validate`
- `/api/config-editor/save`
- `/api/comm/schema`
- `/api/comm/data`
- `/api/comm/validate`
- `/api/comm/save`
- `/api/topology`
- `/api/strategy`
- `/api/strategy/save`
- `/api/strategy/runtime`
- `/api/strategy/logs`
- `/api/system/processes`
- `/api/system/resources`
- `/api/system/logs`
- `/api/system/services/{service}/{action}`
- `/api/users`
- `/api/audit`

### 2.10 用户、权限与审计

当前已经支持：

- PostgreSQL migration 初始化。
- 默认管理员自动创建。
- 用户创建。
- 用户启用、禁用。
- 用户角色设置。
- 登录会话管理。
- 登录、登出审计。
- 用户管理审计。
- 配置保存审计。
- 告警确认、消音审计。
- 遥控遥调下发审计。

当前还没有完善的密码重置、强制改密、密码策略和登录失败锁定。

## 3. 当前已知边界

### 3.1 拓扑仍是表结构编辑

当前拓扑管理的数据模型已经具备，但编辑体验仍依赖配置管理页中的表格。现场运维人员通常更习惯拖拽组件、连线、点选绑定点位的方式。

### 3.2 历史数据仍是 JSONL 文件

历史数据目前适合轻量查询和调试验证。随着点位数量和采样周期增加，JSONL 查询会遇到性能、压缩、归档和检索管理问题。

### 3.3 配置已迁移到 PostgreSQL，CSV 编辑体验仍可改善

当前运行时配置主源已从 CSV 迁移到 PostgreSQL 结构化配置表。CSV 保留为导入、导出、备份格式。

Web 后台提供 `/comm`（CSV 编辑）和 `/config`（结构化配置编辑）两种编辑入口。配置保存后部分 C++ 进程仍需要重启才能完全生效，但告警规则每 30 秒自动刷新。

### 3.4 进程管理仍是脚本级

当前通过多个 PowerShell 脚本启动不同进程，还没有统一的进程管理、健康检查和自动拉起能力。

### 3.5 控制链路还需要加强

当前已经能下发遥控遥调，但现场级能力还需要补：

- 更强的二次确认。
- 操作票或操作原因。
- 回读校验。
- 超时失败处理。
- 命令互锁。
- 权限细分。

### 3.6 自动化测试不足

当前缺少完整测试入口和持续集成验证。核心协议解析、配置校验、控制编码、Web API 权限、拓扑 API 都需要测试保护。

## 4. 后续建议支持的功能

### 4.1 P0：先补稳定交付能力

建议优先做这些：

- 增加一键停止脚本。
- 增加本地验收 checklist。
- 补最小自动化测试。
- 补配置错误定位体验。
- 补控制下发回读校验。
- 补配置保存前 diff 和备份恢复。
- 补拓扑拖拽编辑器的最小闭环。

这些能力会直接提升项目的可交付性。

### 4.2 P1：完善后台运维能力

建议支持：

- 拖拽式拓扑编辑器：
  - 左侧组件库。
  - 画布拖动节点。
  - 拖线生成连接。
  - 点击节点绑定设备和点位。
  - 保存后仍落到现有三张拓扑 CSV。
- 告警规则管理增强：
  - 延时触发。
  - 回差。
  - 恢复条件。
  - 确认意见。
  - 恢复记录。
- 历史趋势曲线。
- 多点历史对比。
- 历史数据 CSV 导出。
- 配置备份恢复。
- 用户改密码。
- 管理员重置密码。
- 操作日志导出。

### 4.3 P2：提升现场运行能力

建议支持：

- 统一进程管理器。
- 进程健康检查。
- 自动重启。
- 程序运行日志按天滚动。
- Web 页面查看日志。
- 通讯状态页面。
- 每台设备的连接状态、失败次数、最后成功时间。
- 每个点位的最后刷新时间、质量位、原始值和工程值。
- 控制命令队列、超时、失败原因。

### 4.4 P3：数据平台化

建议支持：

- 历史数据迁移到 PostgreSQL、TimescaleDB 或其他时序数据库。
- 历史数据保留策略。
- 历史数据压缩。
- 报表导出。
- 日、月、年维度统计。
- 设备运行小时数统计。
- PV 发电量、BESS 充放电量、并网功率统计。

### 4.5 P4：协议和设备扩展

建议支持：

- Modbus RTU。
- MQTT。
- OPC UA。
- HTTP API 接入。
- IEC104 控制方向能力。
- IEC104 总召、对时、遥控确认流程。
- IEC61850 GOOSE。
- 不同厂家设备模板。
- 点表导入导出。

## 5. 推荐下一阶段开发顺序

建议下一阶段不要同时铺太多功能，按下面顺序推进：

1. 拓扑拖拽编辑器最小闭环。
2. 控制链路回读校验和更严格二次确认。
3. 配置保存前 diff 和备份恢复。
4. 历史趋势图和导出。
5. 统一进程管理和健康检查。
6. 最小自动化测试。
7. IEC104 控制与规约细节补齐。
8. IEC61850 GOOSE 或其他协议扩展。

其中最建议先做的是拖拽式拓扑编辑器。原因是当前拓扑数据模型已经落地，继续补拖拽编辑器可以快速把“能展示”提升到“能配置、能交付、现场能用”。

## 6. 小结

OpenEMS 当前已经具备“采集、实时数据、控制、告警、历史、后台管理、审计、拓扑展示、安装运行目录”的基础闭环。后续开发重点不应只是增加页面，而应优先围绕现场交付补齐：

- 配置体验。
- 可观测性。
- 可验证性。
- 操作安全。
- 部署和进程管理。

这些能力完成后，项目会从“能跑的工程”进一步变成“能交付、能维护、能扩展的系统”。
