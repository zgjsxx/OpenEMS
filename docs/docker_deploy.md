# OpenEMS Docker 部署说明

本文档说明如何通过 Docker Compose 在单机上部署 OpenEMS。

## 目录结构

本仓库新增了以下容器化文件：

- `Dockerfile`
- `docker-compose.yml`
- `docker/start_openems.sh`
- `src/web/bootstrap_db.py`

容器采用两服务结构：

- `postgres`：TimescaleDB / PostgreSQL 15
- `openems`：OpenEMS 应用容器，内部同时运行
  - `openems-rtdb-service`
  - `openems-modbus-collector`
  - `openems-iec104-collector`
  - `openems-history`
  - `openems-alarm`
  - FastAPI 后台

这样可以保证 RtDb 共享内存在同一个容器内被所有 OpenEMS 进程共享。

## 启动方式

在仓库根目录执行：

```powershell
docker compose up --build -d
```

查看日志：

```powershell
docker compose logs -f openems
docker compose logs -f postgres
```

停止：

```powershell
docker compose down
```

如果连数据库数据卷也一起删除：

```powershell
docker compose down -v
```

## 访问方式

后台地址：

```text
http://127.0.0.1:8080/login
```

如果你想改宿主机端口，可以在启动前设置：

```powershell
$env:OPENEMS_WEB_PORT = "18080"
docker compose up --build -d
```

默认管理员：

- 用户名：`admin`
- 密码：`admin123`

如果需要覆盖默认管理员用户名，可设置环境变量：

- `OPENEMS_ADMIN_USERNAME`
- `OPENEMS_ADMIN_PASSWORD_HASH`

说明：

- `OPENEMS_ADMIN_PASSWORD_HASH` 需要填写系统当前使用的 PBKDF2 格式哈希值，而不是明文密码。
- 如果数据库里已经存在该用户名，系统不会重复创建默认管理员。

## 初始化逻辑

`openems` 容器启动时会自动执行：

1. 等待 PostgreSQL 就绪
2. 执行 Web migration
3. 自动创建默认管理员
4. 如果结构化配置表为空，则从 `config/tables` 导入初始配置
5. 启动 RtDb、collector、history、alarm、后台

注意：

- 只有“结构化配置表为空”时才会导入 `config/tables`
- 如果你已经在 Web 页面修改过配置，后续重启容器不会被内置 CSV 覆盖

## 常用环境变量

`docker-compose.yml` 中当前支持这些变量：

- `OPENEMS_DB_URL`
- `OPENEMS_ADMIN_USERNAME`
- `OPENEMS_ADMIN_PASSWORD_HASH`
- `OPENEMS_WEB_PORT`
- `OPENEMS_SYNC_CONFIG_ON_START`
- `OPENEMS_ENABLE_MODBUS`
- `OPENEMS_ENABLE_IEC104`
- `OPENEMS_ENABLE_HISTORY`
- `OPENEMS_ENABLE_ALARM`
- `OPENEMS_ENABLE_STRATEGY`

默认行为：

- `OPENEMS_SYNC_CONFIG_ON_START=1`
  仅当 PostgreSQL 结构化配置为空时导入 CSV
- `OPENEMS_ENABLE_MODBUS=1`
- `OPENEMS_ENABLE_IEC104=1`
- `OPENEMS_ENABLE_HISTORY=1`
- `OPENEMS_ENABLE_ALARM=1`
- `OPENEMS_ENABLE_STRATEGY=1`

如果你暂时不想启动某个服务，可以改成 `0`，例如：

```yaml
environment:
  OPENEMS_ENABLE_IEC104: "0"
```

## 数据持久化

Compose 默认创建两个 volume：

- `openems_postgres_data`
- `openems_runtime_data`

其中：

- PostgreSQL 结构化配置、用户、告警、审计、历史数据都保存在 `openems_postgres_data`
- `runtime/` 目录保存在 `openems_runtime_data`

## 配置变更

当前系统运行时只从 PostgreSQL 读取配置，不再从 CSV 读取。

如果你想把仓库里的 `config/tables` 手工导入 PostgreSQL，可以执行：

```powershell
docker compose exec openems python3 ./web/sync_config_to_postgres.py --mode import --config-dir ./config/tables
```

如果你想把 PostgreSQL 当前配置导出为 CSV：

```powershell
docker compose exec openems python3 ./web/sync_config_to_postgres.py --mode export --config-dir ./config/tables
```

## 故障排查

如果后台打不开，优先检查：

1. `docker compose ps`
2. `docker compose logs -f openems`
3. `docker compose logs -f postgres`

重点关注以下问题：

- `OPENEMS_DB_URL` 配置错误
- PostgreSQL 尚未 ready
- `config/tables` 初始 CSV 校验失败
- 现场协议连接地址在容器里不可达

## 说明

这个 Docker 方案使用 Linux 容器运行 OpenEMS。即使你在 Windows 上使用 Docker Desktop，容器内部仍是 Linux 环境。
