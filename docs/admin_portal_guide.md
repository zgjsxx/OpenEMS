# OpenEMS Admin Portal 使用说明

本文档介绍 OpenEMS 当前实现的单站运维后台 `admin portal`，包括其架构定位、依赖、启动方式、页面功能、测试步骤和当前边界。

## 1. 功能定位

当前后台基于 `FastAPI + 多页面 HTML/JS` 实现，目标是为单站 OpenEMS 提供一套运维入口，而不是替代现有采集进程。

当前版本已经接入的能力包括：

- 登录认证
- 用户管理
- 角色权限控制
- 告警管理
- 通讯配置管理
- 配置概览
- 历史查询
- 审计日志
- 实时看板
- 遥控遥调审计
- 拓扑管理
- 策略引擎管理
- 系统监控

后台管理数据统一落在 `PostgreSQL`，而实时采集数据和历史数据仍沿用项目原有链路：

- 实时点位：`RtDb` 共享内存
- 活动告警：PostgreSQL `alarm_events` 表（告警进程直接写入，不再通过 `alarms_active.json`）
- 历史数据：`runtime/history/*.jsonl`
- 现场配置：PostgreSQL 结构化配置表（CSV 保留为导入、导出和备份格式）

也就是说，这套后台是”管理层 + 展示层”，不是采集核心本身。

## 2. 代码位置

后台相关代码主要位于：

- `src/web/admin_server.py`
- `src/web/run_dashboard.py`
- `src/web/db.py`
- `src/web/auth.py`
- `src/web/config_store.py`

页面文件位于：

- `src/web/login.html`
- `src/web/dashboard_admin.html`
- `src/web/alarms_admin.html`
- `src/web/history_admin.html`
- `src/web/config_admin.html`
- `src/web/comm_admin.html`
- `src/web/topology_admin.html`
- `src/web/strategy_admin.html`
- `src/web/system_admin.html`
- `src/web/users_admin.html`
- `src/web/audit_admin.html`

数据库 migration 位于：

- `src/web/migrations/001_init.sql`

前端公共资源位于：

- `src/web/assets/admin.css`
- `src/web/assets/admin.js`

## 3. 运行依赖

### 3.1 Python 依赖

当前后台依赖定义在：

- `src/web/requirements.txt`

内容包括：

- `fastapi`
- `uvicorn`
- `psycopg[binary]`

建议优先使用虚拟环境，不要直接把依赖装到系统 Python。

推荐步骤：

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -i https://pypi.tuna.tsinghua.edu.cn/simple -r src/web/requirements.txt
```

如果你使用的是其他国内镜像，也可以替换为例如：

- 阿里云：`https://mirrors.aliyun.com/pypi/simple/`
- 腾讯云：`https://mirrors.cloud.tencent.com/pypi/simple/`

如果只是临时安装一次，用 `-i` 参数即可；如果希望长期生效，可以单独配置 `pip.ini`。

### 3.2 PostgreSQL

后台强依赖 PostgreSQL。

如果数据库不可用：

- 登录不可用
- 后台 API 不可用
- 页面会无法进入或返回数据库错误

但这不应该影响采集进程本身，因为采集主链路并不依赖这套后台。

### 3.3 环境变量

当前最关键的环境变量是：

- `OPENEMS_DB_URL`

示例：

```powershell
$env:OPENEMS_DB_URL="postgresql://postgres:your_password@127.0.0.1:5432/openems_admin"
```

可选管理员初始化变量：

- `OPENEMS_ADMIN_USERNAME`
- `OPENEMS_ADMIN_PASSWORD_HASH`

如果不提供管理员相关变量，数据库首次初始化时会自动创建默认管理员：

- 用户名：`admin`
- 密码：`admin123`

## 4. 启动方式

当前后台启动脚本为：

- `src/web/run_dashboard.py`

启动命令：

```powershell
python src/web/run_dashboard.py --port 8080
```

启动后默认访问地址：

- `http://localhost:8080/login`

当前启动脚本已经指向新的后台入口：

- `uvicorn.run("admin_server:app", ...)`

为兼容旧用法，`src/web/server.py` 也已经转发到新的 `admin_server:app`。

## 5. 数据与职责边界

### 5.1 PostgreSQL 承担的内容

当前 PostgreSQL 负责：

- 用户表 `users`
- 登录会话 `user_sessions`
- 告警事件表 `alarm_events`
- 审计日志 `audit_logs`
- 系统设置表 `app_settings`
- migration 记录表 `schema_migrations`

### 5.2 仍然保留文件或共享内存的数据源

当前这些数据并没有完全迁移到数据库：

- `config/tables/*.csv`：CSV 保留为导入、导出、备份和初始 bootstrap 格式，运行时配置从 PostgreSQL 读取
- `runtime/history/*.jsonl`：历史数据仍以 JSONL 文件存储
- 共享内存 `RtDb`：实时点位和命令状态

所以后台的实现方式是：

- 从 PostgreSQL 读取并编辑通讯配置
- 从 PostgreSQL 读取活动告警（告警进程已直接写入 PostgreSQL）
- 从历史 JSONL 文件查询历史
- 从共享内存读取实时点位和命令状态

## 6. 页面说明

### 6.1 `/login`

登录页，使用本地用户名和密码登录。

后端接口：

- `POST /api/auth/login`
- `POST /api/auth/logout`
- `GET /api/auth/me`

### 6.2 `/dashboard`

首页看板，聚合展示：

- 数据库状态
- 设备数量
- 点位统计
- 活动告警预览
- 实时设备点位
- 遥控遥调入口

涉及接口：

- `GET /api/system/status`
- `GET /api/snapshot`
- `GET /api/alarms/active`
- `POST /api/command`
- `GET /api/command/{point_id}`

### 6.3 `/alarms`

告警管理页。

当前支持：

- 查看活动告警
- 查看历史告警
- 按状态、级别、设备过滤
- 确认告警
- 消音告警

涉及接口：

- `GET /api/alarms`
- `POST /api/alarms/{alarm_id}/ack`
- `POST /api/alarms/{alarm_id}/silence`

### 6.4 `/history`

历史查询页。

当前支持：

- 选择点位
- 选择时间范围
- 查询 `runtime/history/*.jsonl`

涉及接口：

- `GET /api/history/points`
- `GET /api/history/query`
- `GET /api/history/query_multi`
- `GET /api/history/query_aggregate`

### 6.5 `/config`

配置概览页。

当前用于展示：

- 设备数量
- 协议分布
- 点位总量
- 映射数量
- 设备列表

涉及接口：

- `GET /api/config/overview`
- `GET /api/config`

### 6.6 `/comm`

通讯配置页。

当前支持在线编辑这些配置表：

- `device.csv`
- `telemetry.csv`
- `teleindication.csv`
- `telecontrol.csv`
- `teleadjust.csv`
- `modbus_mapping.csv`
- `iec104_mapping.csv`
- 以及 `site.csv`、`ems_config.csv`
- `alarm_rule.csv`

涉及接口：

- `GET /api/comm/schema`
- `GET /api/comm/data`
- `POST /api/comm/validate`
- `POST /api/comm/save`

保存前会做校验，保存时会自动备份当前 CSV 到：

- `runtime/config_backups/<timestamp>/`

### 6.7 `/config`

配置编辑页（config-editor），与 `/comm` 不同，此页面直接操作 PostgreSQL 结构化配置表。

涉及接口：

- `GET /api/config-editor/schema`
- `GET /api/config-editor/data`
- `POST /api/config-editor/validate`
- `POST /api/config-editor/save`
- `GET /api/config/overview`
- `GET /api/config`

### 6.8 `/topology`

拓扑管理页，展示站点电气单线图。

当前支持：

- SVG 单线图展示
- 节点、线路、点位绑定配置
- 实时数据叠加展示
- 点击节点查看绑定点位、实时值和告警

涉及接口：

- `GET /api/topology`

### 6.9 `/strategy`

策略引擎管理页，管理防逆流和 SOC 保护策略。

当前支持：

- 查看策略定义、点位绑定和参数配置
- 修改策略配置并保存
- 查看策略运行状态
- 查看策略动作日志

涉及接口：

- `GET /api/strategy`
- `POST /api/strategy/save`
- `GET /api/strategy/runtime`
- `GET /api/strategy/logs`

### 6.10 `/system`

系统监控页，查看进程状态、资源使用和日志。

当前支持：

- 查看运行中的进程列表
- 查看系统资源使用（CPU、内存、磁盘）
- 查看服务日志
- 控制服务启停（启动/停止）

涉及接口：

- `GET /api/system/processes`
- `GET /api/system/resources`
- `GET /api/system/logs`
- `POST /api/system/services/{service}/{action}`

### 6.11 `/users`

用户管理页，只允许管理员访问。

当前支持：

- 新建用户
- 启用 / 禁用用户
- 设置角色

涉及接口：

- `GET /api/users`
- `POST /api/users`
- `PATCH /api/users/{user_id}`

### 6.12 `/audit`

审计日志页，只允许管理员访问。

当前支持查询：

- 登录
- 登出
- 用户创建
- 用户修改
- 告警确认
- 告警消音
- 通讯配置保存
- 遥控遥调提交

涉及接口：

- `GET /api/audit`

## 7. 权限模型

当前固定三种角色：

- `viewer`
- `operator`
- `admin`

权限大致如下：

- `viewer`
  - 可查看首页、告警、历史、配置概览
  - 不允许下发命令
  - 不允许修改通讯配置
  - 不允许访问用户和审计管理页

- `operator`
  - 继承 `viewer`
  - 可执行遥控遥调
  - 可确认 / 消音告警
  - 仍不能改通讯配置
  - 仍不能访问用户管理和审计管理

- `admin`
  - 拥有全部权限
  - 可管理用户
  - 可修改通讯配置
  - 可查看审计日志

## 8. 通讯配置校验能力

当前 `src/web/config_store.py` 已经实现较完整的校验逻辑，包括：

- 必填字段校验
- 单行表数量校验
- 站点引用校验
- 设备 ID 唯一性
- 点位 ID 全局唯一性
- 设备协议与映射协议一致性
- `writable` 合法性校验
- `telecontrol` / `teleadjust` 必须 `writable=true`
- `modbus_mapping.data_type` 与点表 `data_type` 一致性
- `register_count` 与数据类型匹配
- 读点 / 写点功能码约束
- Modbus 与 IEC104 同一点位映射冲突检查

因此当前后台不只是“能编辑 CSV”，而是“带约束的配置编辑器”。

## 9. 当前建议的测试步骤

建议按下面顺序测试。

### 9.1 基础启动测试

1. 安装 Python 依赖
2. 准备 PostgreSQL 数据库
3. 设置 `OPENEMS_DB_URL`
4. 启动：

```powershell
python src/web/run_dashboard.py --port 8080
```

5. 打开：

- `http://localhost:8080/login`

6. 使用默认管理员登录：

- 用户名：`admin`
- 密码：`admin123`

### 9.2 后台功能测试

#### 用户管理

1. 进入 `/users`
2. 新建一个 `viewer`
3. 新建一个 `operator`
4. 禁用一个用户
5. 用被禁用用户尝试登录，确认被拒绝

#### 审计日志

1. 进入 `/audit`
2. 确认出现：
   - `login`
   - `user_create`
   - `user_update`

#### 通讯配置

1. 进入 `/comm`
2. 执行一次校验
3. 故意写入一个非法值，例如非法端口
4. 再次校验，确认能收到错误
5. 改回正确值并保存
6. 检查 `runtime/config_backups/` 是否生成备份
7. 再去 `/audit` 检查是否留下 `comm_save`

#### 告警管理

前提：`runtime/alarms_active.json` 中有活动告警。

1. 进入 `/alarms`
2. 查看活动告警
3. 执行确认
4. 执行消音
5. 去 `/audit` 确认有 `alarm_ack` / `alarm_silence`

### 9.3 联调测试

如果要测首页实时数据与遥控遥调，还需要启动采集侧进程，例如：

- `openems-modbus-collector`
- 或 `openems-iec104-collector`

如果要测历史页，还需要：

- `runtime/history/*.jsonl` 中已有历史数据

如果要测活动告警页，还需要：

- `runtime/alarms_active.json` 中已有告警内容

## 10. 当前已知边界

当前这版后台已经可用，但仍有一些现实边界需要注意。

### 10.1 不是完整的多站点平台

当前只面向单站，不支持：

- 多租户
- 多站点切换
- 组织级权限隔离

### 10.2 配置主源已迁移到 PostgreSQL

当前运行时配置主源已从 CSV 迁移到 PostgreSQL 结构化配置表。

CSV 保留为导入、导出、备份和初始 bootstrap 格式。`/comm` 页面编辑 CSV，`/config` 页面直接操作 PostgreSQL 结构化配置表。

### 10.3 实时数据仍依赖采集进程

后台不会自己生成实时数据。

如果采集器没启动：

- 首页实时点位为空
- 命令状态不可用
- 历史与告警也可能没有数据

### 10.4 PostgreSQL 不可用时后台无法工作

当前登录和管理功能都依赖数据库。

如果数据库不可用，后台会启动失败或接口返回 503，这是当前设计下的预期行为。

### 10.5 中文显示问题

如果在某些 PowerShell 窗口中查看 HTML 源文件时看到中文乱码，通常是终端显示编码问题，不代表文件实际保存有问题。浏览器中通常会正常显示。

## 11. 后续建议

这版后台已经具备主骨架，但如果继续迭代，建议优先做下面几件事：

1. 补真实端到端测试
2. 增加 PostgreSQL 连接失败时的更友好提示
3. 完善用户修改密码能力
4. 为通讯配置页增加更细的错误定位
5. 把告警历史和活动态同步策略进一步做稳
6. 补部署文档与初始化脚本

## 12. 小结

当前 `admin portal` 已经不是一个简单的只读仪表板，而是一套具备以下能力的单站运维后台：

- 有登录
- 有权限
- 有管理数据落库
- 有告警处理
- 有配置编辑
- 有审计
- 能挂接现有采集和历史链路

它已经可以作为后续 Web 平台化改造的基础版本继续演进。
