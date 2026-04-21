# AGENTS.md

本文档用于给 Codex 一类代码代理提供最小但足够的项目上下文，帮助其在本仓库中更安全、更高效地协作。

## 项目概览

OpenEMS 是一个基于 CMake 构建的 C++17 能源管理 / 遥测采集项目。

从当前仓库结构看，已经实现或较完整的模块主要包括：

- `src/common`：公共类型、常量、错误码、线程安全队列、基础接口
- `src/utils`：日志与时间工具
- `src/model`：站点、设备、点位及映射模型
- `src/config`：基于 CSV 的配置加载
- `src/modbus`：Modbus TCP 客户端与数据解析
- `src/iec104`：IEC 104 客户端与 ASDU 处理
- `src/collector`：轮询与控制服务
- `src/rt_data`：实时数据管理
- `src/rt_db`：实时数据库层
- `src/apps`：可执行程序入口
- `src/web`：Python 仪表板与共享内存查看辅助脚本

仓库中也存在一些目录已创建但暂未完全接入或尚在演进中，例如：

- `src/alarm`
- `src/control`
- `src/core`
- `src/strategy`

## 构建系统

当前主要构建方式：

- `CMake >= 3.16`
- `C++17`
- Windows 下预计使用 Visual Studio 或 Ninja 生成器

常用本地构建命令：

```powershell
cmake -S . -B build
cmake --build build
```

构建产物默认输出到：

- `build/bin`
- `build/lib`

构建过程中会将 `config/` 目录复制到构建目录，供运行时使用。

## 主要可执行程序

当前 `src/apps/CMakeLists.txt` 中定义了以下可执行文件：

- `openems-modbus-collector`
- `openems-viewer`
- `openems-alarm`
- `openems-iec104-collector`

如果任务涉及运行时行为修改，优先检查对应的入口文件：

- `src/apps/src/collector_main.cpp`
- `src/apps/src/viewer_main.cpp`
- `src/apps/src/alarm_main.cpp`
- `src/apps/src/iec104_collector_main.cpp`

## 配置数据

当前运行时 CSV 配置位于：

- `config/tables/device.csv`
- `config/tables/ems_config.csv`
- `config/tables/iec104_mapping.csv`
- `config/tables/modbus_mapping.csv`
- `config/tables/teleadjust.csv`
- `config/tables/site.csv`
- `config/tables/telecontrol.csv`
- `config/tables/teleindication.csv`
- `config/tables/telemetry.csv`

如果调整 CSV 字段、格式或语义，需要同步检查 `src/config` 中的加载逻辑。

## 测试现状

仓库中存在 `tests/` 目录结构，但从当前已检出的顶层构建文件来看，还没有看到完整接入的统一测试入口。

对代理的建议：

- 如果目标模块已经有可运行的测试目标，优先补充或更新针对性测试。
- 如果当前没有现成可运行测试入口，至少构建受影响目标，并在最终说明里明确指出测试覆盖缺口。

## 协作约定

- 优先做小而聚焦的模块内修改，避免无关的大范围重构。
- 修改实现时，保持对应公开头文件与 `src/` 中实现同步。
- 默认延续现有 C++17 风格、命名方式和模块边界，除非任务明确要求整理风格。
- 涉及协议解析、点表映射、配置装载时，要结合 `config/tables` 中的 CSV 一起核对。
- 涉及 Windows 网络功能时，注意部分模块依赖平台库，例如 `ws2_32`。

## 当前仓库的几个现实情况

在做较大修改前，建议先注意以下几点：

- 顶层 `CMakeLists.txt` 当前只接入了 `src/` 下的一部分模块，目录存在不代表已经参与构建。
- 部分 CMake 注释存在编码异常，除非任务相关，否则不要为了“顺手清理”去重写无关文件。
- `README.md` 当前内容很少，判断行为时应更多依赖源码结构而不是现有说明文档。

## 代理默认工作流程

1. 先阅读目标模块的 `CMakeLists.txt`、公开头文件和程序入口。
2. 用最小且完整的改动解决问题。
3. 尽量只构建受影响的最小目标。
4. 明确说明未覆盖的构建、运行或测试风险。

## 按任务类型优先查看的位置

- Modbus 相关问题：`src/modbus`、`src/model`、`config/tables/modbus_mapping.csv`
- IEC104 相关问题：`src/iec104`、`src/model`、`config/tables/iec104_mapping.csv`
- 配置相关问题：`src/config`、`config/tables`
- 实时数据链路问题：`src/collector`、`src/rt_data`、`src/rt_db`、`src/apps`
- 仪表板或查看器问题：`src/web` 及相关入口文件
