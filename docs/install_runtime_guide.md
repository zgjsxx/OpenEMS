# OpenEMS install 运行目录说明

本文档说明如何把 OpenEMS 安装到仓库根目录下的 `install/`，并以 `install/` 作为统一运行根目录。

## 目标

统一以下运行时路径：

- 可执行程序：`install/bin/`
- 配置文件：`install/config/`
- 运行时数据：`install/runtime/`
- Web 后台：`install/web/`

这样可以避免以下分叉问题：

- 采集程序写入 `build/.../runtime/`
- Web 页面却读取仓库根或其他目录下的 `runtime/`

## 安装步骤

推荐使用现有 VS2019 构建目录：

```powershell
cmake -S . -B build-codex-vs2019 -G "Visual Studio 16 2019" -A x64
cmake --build build-codex-vs2019 --config Release
cmake --install build-codex-vs2019 --config Release
```

执行完成后，仓库根目录会生成：

```text
install/
  bin/
  config/
  runtime/
    history/
    config_backups/
  web/
  start_admin_portal.ps1
  start_history.ps1
  start_alarm.ps1
  start_modbus_collector.ps1
  start_iec104_collector.ps1
  init_admin_portal_db.ps1
```

## 运行约定

后续推荐统一从 `install/` 根目录运行，不再直接在 `build/.../bin/Release` 下裸跑 exe。

原因是当前默认路径都按 `install/` 根目录来解释：

- `config/tables`
- `runtime/history`
- `runtime/alarms_active.json`
- `runtime/config_backups`

## 推荐启动方式

### 1. 启动 Modbus 采集

```powershell
.\install\start_modbus_collector.ps1
```

### 2. 启动 IEC104 采集

```powershell
.\install\start_iec104_collector.ps1
```

### 3. 启动历史采样

```powershell
.\install\start_history.ps1
```

历史数据默认写入：

- `install/runtime/history/`

### 4. 启动告警程序

```powershell
.\install\start_alarm.ps1
```

活动告警默认写入：

- `install/runtime/alarms_active.json`

### 5. 初始化后台数据库

如果本地已经有 PostgreSQL 容器，可以先执行：

```powershell
.\install\init_admin_portal_db.ps1
```

这个脚本负责建库；表会在后台启动时自动创建。

### 6. 启动 Web 后台

```powershell
.\install\start_admin_portal.ps1
```

默认访问地址：

- `http://localhost:8080/login`

## Web 后台路径说明

安装后的 Web 后台采用以下规则：

- 页面和静态资源从 `install/web/` 加载
- 通讯配置从 `install/config/tables/` 读取
- 历史数据从 `install/runtime/history/` 读取
- 活动告警从 `install/runtime/alarms_active.json` 读取
- 配置备份写入 `install/runtime/config_backups/`

也就是说，只要通过 `install/` 下的启动脚本运行，Web 和 C++ 程序就会共享同一套目录。

## 兼容性说明

当前仍然保留原有命令行参数覆盖能力，例如：

- `openems-modbus-collector [config_path]`
- `openems-iec104-collector [config_path]`
- `openems-alarm [shm_name] [output_path]`
- `openems-history [shm_name] [history_dir] [interval_ms]`

所以如果确实需要，也仍然可以手动指定其他目录。

但默认推荐方式已经变成：

- 通过 `install/` 启动脚本运行
- 让所有进程共享同一个 `install/` 目录树

## 常见问题

### 为什么页面看不到历史数据？

通常是因为历史程序写到了别的工作目录，例如：

- `build-codex-vs2019/bin/Release/runtime/history/`

而 Web 页面读的是：

- `install/runtime/history/`

统一改成从 `install/` 启动后，这个问题就不会再反复出现。

### 为什么页面看不到活动告警？

原因类似，通常是告警程序写入的 `runtime/alarms_active.json` 和 Web 后台读取的不是同一个目录。

## 建议

后续日常联调、演示和部署，尽量都基于 `install/` 目录进行：

1. 先 `cmake --install`
2. 再通过 `install/*.ps1` 启动各程序
3. 不再把 `build/` 目录当运行目录使用
