# OpenEMS

OpenEMS 是一个单站能源管理 / 遥测采集项目，当前包含 C++ 采集与 RtDb 进程，以及 Python FastAPI 运维后台。

## 运行目录

推荐使用 CMake install 生成仓库根目录下的 `install/`，并从 `install/` 运行所有程序：

```powershell
cmake -S . -B build-codex-vs2019 -G "Visual Studio 16 2019" -A x64
cmake --build build-codex-vs2019 --config Release
cmake --install build-codex-vs2019 --config Release --prefix install
```

安装后主要目录：

- `install/bin`
- `install/config`
- `install/runtime`
- `install/web`

## PostgreSQL

Web 后台和 `openems-rtdb-service` 都通过 `OPENEMS_DB_URL` 连接 PostgreSQL。

本地默认示例：

```powershell
$env:OPENEMS_DB_URL = "postgresql://postgres:postgres@127.0.0.1:5432/openems_admin"
```

当前按 PostgreSQL 15 验证。Python 后台依赖安装：

```powershell
python -m pip install -r src/web/requirements.txt
```

## 结构化配置

PostgreSQL 是主配置源，配置按结构化表存储，例如：

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

旧的 `config_tables(table_name, rows_json)` 已不再作为运行配置源。迁移脚本会把旧 JSON 快照拆入结构化表，并将旧表改名为 `config_tables_legacy`。

CSV 保留为导入、导出、备份和离线兜底格式。

## RtDb Service 配置源

`openems-rtdb-service` 从 PostgreSQL 结构化配置表读取运行必需配置：

- `sites`
- `ems_config`
- `devices`
- `points`
- `modbus_mappings`
- `iec104_mappings`

运行时仅支持 PostgreSQL 配置源。如果 PostgreSQL 不可用或 `libpq` 运行库不存在，服务将启动失败。

启动参数：

```text
openems-rtdb-service [shm_name]
```

参数说明：

- `shm_name`：共享内存名称，默认使用平台内置值

示例：

```powershell
.\bin\openems-rtdb-service.exe
```

Linux 示例：

```sh
./bin/openems-rtdb-service
```

## CSV 导入导出

CSV 保留为导入、导出、备份和离线兜底格式。

安装目录下导入 CSV 到 PostgreSQL：

```powershell
.\sync_config_to_postgres.ps1
```

Linux：

```sh
./sync_config_to_postgres.sh
```

源码目录开发调试：

```powershell
python src/web/sync_config_to_postgres.py --mode import --config-dir config/tables
python src/web/sync_config_to_postgres.py --mode export --config-dir config/tables
```

## libpq 运行库

RtDb service 使用运行时动态加载 `libpq`。

- Windows：`third_party/postgresql/windows/x64/bin/` 中放置 `libpq.dll` 及依赖 DLL，install 时会复制到 `install/bin`。
- Linux：`third_party/postgresql/linux/x64/lib/` 中放置 `libpq.so*`，install 时会复制到 `install/lib`。

如果运行库不可用，程序会打印 warning 并退出。
## Docker 部署

仓库已提供：

- `Dockerfile`
- `docker-compose.yml`

可直接启动：

```powershell
docker compose up --build -d
```

后台默认地址：

```text
http://127.0.0.1:8080/login
```

详细说明见：

- `docs/docker_deploy.md`
