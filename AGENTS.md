# AGENTS.md

本文档为代码代理提供当前仓库的最小项目上下文。

## 项目概览

OpenEMS 是一个基于 CMake 构建的 C++17 能源管理 / 遥测采集项目，当前已包含：

- `src/common`：公共类型、错误码、并发工具
- `src/utils`：日志与时间工具
- `src/model`：站点、设备、点位模型
- `src/config`：配置装载与 PostgreSQL 配置读取
- `src/modbus`：Modbus TCP 通讯
- `src/iec104`：IEC 104 通讯
- `src/collector`：轮询与控制服务
- `src/strategy`：策略算法
- `src/rt_db`：实时库与共享内存表空间
- `src/apps`：可执行程序入口
- `src/web`：FastAPI 运维后台

## 构建系统

- `CMake >= 3.16`
- `C++17`

Windows 常用构建命令：

```powershell
cmake -S . -B build
cmake --build build
```

## 主要可执行程序

- `openems-rtdb-service`
- `openems-modbus-collector`
- `openems-iec104-collector`
- `openems-history`
- `openems-alarm`
- `openems-strategy-engine`
- `openems-viewer`

## 配置与持久化

当前运行时配置统一存放在 PostgreSQL 结构化表中，初始化通过 SQL migration / seed 完成。

不再依赖 `config/tables` 下的 CSV 文件作为运行时配置源。

主要结构化配置表：

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

## RtDb

RtDb 已演进为按表拆分的共享内存表空间，核心表包括：

- `catalog`
- `point_index`
- `telemetry`
- `teleindication`
- `command`
- `strategy_runtime`
- `alarm_active`

如果任务涉及实时库，请优先检查：

- `src/rt_db/include/openems/rt_db/rt_db_layout.h`
- `src/rt_db/include/openems/rt_db/rt_db.h`
- `src/rt_db/src/rt_db.cpp`
- `src/web/shm_reader.py`

## 测试建议

- 优先补充或更新受影响模块的单元测试
- 至少构建受影响目标
- 若涉及 RtDb / Docker / Web 联调，尽量补一轮最小闭环验证

## 协作约定

- 优先做小而聚焦的改动
- 修改实现时保持头文件与源文件同步
- 默认延续现有 C++17 风格和模块边界
- 涉及协议、点位、共享内存布局时，优先核对源码和 SQL schema
- Windows 网络相关注意平台库，例如 `ws2_32`

## 按任务类型优先查看的位置

- Modbus：`src/modbus`、`src/model`
- IEC104：`src/iec104`、`src/model`
- 配置：`src/config`、`src/web/db.py`、`src/web/migrations`
- 实时数据链路：`src/collector`、`src/rt_db`、`src/apps`
- Web：`src/web`
