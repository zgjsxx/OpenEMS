# OpenEMS

## Web 管理后台数据库

当前仓库中的 Web 管理后台使用 PostgreSQL。

- 服务端默认按 PostgreSQL 15 配置。
- 依据脚本：
  - [src/web/init_admin_portal_db.ps1](src/web/init_admin_portal_db.ps1)
  - [src/web/install_scripts/init_admin_portal_db.ps1](src/web/install_scripts/init_admin_portal_db.ps1)
- 这两个脚本里使用的容器名都是 `postgres15`。

本地启动管理后台时，数据库连接串通过 `OPENEMS_DB_URL` 注入，相关脚本：

- [src/web/start_admin_portal_local.ps1](src/web/start_admin_portal_local.ps1)
- [src/web/install_scripts/start_admin_portal.ps1](src/web/install_scripts/start_admin_portal.ps1)

Python 驱动层位于 [src/web/db.py](src/web/db.py)：

- 优先使用 `psycopg`
- 如果没有安装，再回退到 `psycopg2`

说明：

- 当前仓库中明确的是“默认 PostgreSQL 服务端版本为 15”
- Python 驱动版本没有在仓库里固定死，只约定驱动类型为 `psycopg` 或 `psycopg2`
