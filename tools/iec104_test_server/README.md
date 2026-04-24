# IEC104 Python Test Server

这个目录提供一个独立的 IEC104 辅助测试上位机，不属于 OpenEMS 主系统运行时模块。

它基于 Python 的 `c104` 库启动一个 IEC 60870-5-104 server，同时提供本地 Web 配置界面，便于：

- 导入 `config/tables/iec104_mapping.csv`
- 编辑站点和点位
- 修改点值并手动发送
- 周期上送监视点
- 接收并回执遥控/设点命令
- 查看连接状态和最近事件日志

## 目录

- `app.py`: 本地 Web UI 与 API 入口
- `server.py`: IEC104 server 运行管理器
- `config.example.json`: 示例配置
- `runtime/config.json`: 实际运行配置，首次启动会自动创建
- `runtime/events.jsonl`: 可选事件日志

## 安装

```powershell
cd tools\iec104_test_server
python -m pip install -r requirements.txt
```

## 启动

```powershell
cd tools\iec104_test_server
python app.py
```

默认情况下：

- Web UI: `http://127.0.0.1:8094`
- IEC104 server: `0.0.0.0:2404`

这些参数都可以在页面顶部修改并保存到 `runtime/config.json`。

## 推荐联调步骤

1. 启动这个测试上位机：`python app.py`
2. 打开 `http://127.0.0.1:8094`
3. 点击“导入 iec104_mapping.csv”
4. 视需要调整点位、值、自动上送周期
5. 点击“启动 Server”
6. 再启动 OpenEMS 的 `openems-iec104-collector.exe`
7. 观察：
   - 测试上位机的连接状态和事件日志
   - OpenEMS 采集日志
   - dashboard / viewer 中的 RtDb 值变化

## 配置说明

配置文件使用 JSON，主要字段：

- `server`
  - `ip`, `port`, `tick_rate_ms`, `ui_port`
  - `persist_events`: 是否把事件写到 `runtime/events.jsonl`
  - `force_fail_commands`: 是否强制命令失败
- `stations`
  - `common_address`, `name`
- `points`
  - `id`
  - `station_common_address`
  - `ioa`
  - `type_id`
  - `type`
  - `category`
  - `value`
  - `quality`
  - `report_ms`
  - `auto_transmit`
  - `writable`
  - `related_ioa`
  - `scale`
  - `description`

## 注意

- 这是测试工具，默认只用于本仓库联调环境。
- `c104` 为 GPLv3 许可，当前按辅助测试程序使用，不并入 OpenEMS 主系统代码路径。
- 结构性配置变更保存后，需要点击“重启 Server”生效。
