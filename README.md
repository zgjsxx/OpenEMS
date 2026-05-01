# OpenEMS

OpenEMS 是一个单站 EMS / 遥测采集 / 运维后台项目，当前主要由以下部分组成：

- C++ 采集、实时库、告警、历史、策略引擎
- Python FastAPI 运维后台
- PostgreSQL / TimescaleDB 持久化
- Docker 部署与 E2E 仿真环境

## 构建

Windows:

```powershell
cmake -S . -B build-codex-vs2019 -G "Visual Studio 16 2019" -A x64
cmake --build build-codex-vs2019 --config Release
cmake --install build-codex-vs2019 --config Release --prefix install
```

Linux:

```sh
cmake -S . -B build
cmake --build build
cmake --install build --prefix install
```

## 运行目录

推荐通过 `install/` 目录运行：

- `install/bin`
- `install/config`
- `install/runtime`
- `install/web`

## PostgreSQL

系统运行时配置和业务数据统一存放在 PostgreSQL / TimescaleDB 中。

常用环境变量：

```powershell
$env:OPENEMS_DB_URL = "postgresql://postgres:postgres@127.0.0.1:5432/openems_admin"
```

Python 后台依赖安装：

```powershell
python -m pip install -r src/web/requirements.txt
```

## 配置存储

当前运行时只使用 PostgreSQL 结构化配置表，不再依赖 `config/tables` 下的 CSV 文件。

主要结构化配置表包括：

- `sites`
- `ems_config`
- `devices`
- `points`
- `modbus_mappings`
- `iec104_mappings`
- `alarm_rules`
- `topology_nodes`
- `topology_links`
- `topology_bindings`

初始化方式：

- 数据库 schema 通过 `src/web/migrations/*.sql` 自动初始化
- 示例 / 测试数据通过 SQL seed 导入

## RtDb

RtDb 当前采用按表拆分的共享内存表空间模型，核心表包括：

- `catalog`
- `point_index`
- `telemetry`
- `teleindication`
- `command`
- `strategy_runtime`
- `alarm_active`

`openems-rtdb-service` 从 PostgreSQL 结构化表读取配置并创建这些共享内存表。

## Docker

仓库提供：

- `Dockerfile`
- `docker-compose.yml`
- `docker-compose.e2e.yml`

启动：

```powershell
docker compose up --build -d
```

E2E 仿真环境：

```powershell
docker compose -f docker-compose.yml -f docker-compose.e2e.yml up --build -d
```

后台默认地址：

- [http://127.0.0.1:8080/login](http://127.0.0.1:8080/login)

更多说明见：

- [docs/docker_deploy.md](docs/docker_deploy.md)
- [docs/strategy_e2e_guide.md](docs/strategy_e2e_guide.md)
