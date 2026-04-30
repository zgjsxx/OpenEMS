# Modbus Collector 遥信遥测采集与遥控遥调下发说明

本文档介绍 `openems-modbus-collector` 在当前仓库中的工作方式，重点说明：

- 如何采集 Modbus 设备的遥信、遥测数据
- 如何通过共享内存 / Web 接口下发遥控、遥调命令
- 配置文件如何决定采集点和控制点

本文档基于当前代码实现整理，核心入口与实现位于：

- `src/apps/src/collector_main.cpp`
- `src/collector/src/polling_service.cpp`
- `src/collector/src/control_service.cpp`
- `src/rt_db/src/rt_db.cpp`
- `src/web/admin_server.py`
- `src/web/shm_reader.py`

## 1. 总体架构

`openems-modbus-collector` 启动后，会完成以下几件事：

1. 从 `config/tables` 加载站点、设备、点位和 Modbus 映射配置。
2. 仅筛选 `protocol = modbus-tcp` 的设备参与本进程处理。
3. 创建共享内存实时库 `Global\openems_rt_db`。
4. 为每个 Modbus 设备创建一个 `ModbusTcpClient` 连接。
5. 为每个设备启动一个采集线程，周期性轮询点位数据。
6. 如果设备上存在可写点，则再为该设备启动一个控制线程，用于消费命令槽中的待执行命令并下发到设备。

可以把它理解成两个并行子系统：

- 采集链路：设备 -> Modbus 读功能码 -> 数据解析 -> RtDb
- 控制链路：外部系统 -> RtDb 命令槽 -> Modbus 写功能码 -> 回读校验 -> RtDb 命令状态

## 2. 配置文件说明

### 2.1 设备配置

设备定义在 `config/tables/device.csv`，关键字段如下：

- `id`：设备 ID
- `site_id`：所属站点
- `protocol`：通信协议，`modbus-tcp` 才会被 `modbus-collector` 处理
- `ip` / `port`：设备地址
- `unit_id`：Modbus 从站地址
- `poll_interval_ms`：设备轮询周期

示例：

```csv
id,site_id,name,type,protocol,ip,port,unit_id,poll_interval_ms,common_address
pv-001,site-demo-001,PV Inverter #1,PV,modbus-tcp,127.0.0.1,502,1,1000,
bess-001,site-demo-001,BESS #1,BESS,modbus-tcp,127.0.0.1,5021,1,500,
```

### 2.2 点位分类

点位分散定义在几张 CSV 表中：

- `telemetry.csv`：遥测点
- `teleindication.csv`：遥信点
- `telecontrol.csv`：遥控点
- `teleadjust.csv`：遥调点

公共字段含义：

- `id`：点位 ID
- `device_id`：所属设备
- `data_type`：数据类型，如 `bool`、`uint16`、`int32`、`float32`
- `unit`：工程单位
- `writable`：是否允许写入

注意：

- `telecontrol.csv` 和 `teleadjust.csv` 在当前实现中是可选的。
- 即使 `writable=true`，也只有在 `modbus_mapping.csv` 中配置了写功能码，才会真正被注册为可下发命令点。

### 2.3 Modbus 映射配置

`config/tables/modbus_mapping.csv` 决定点位如何映射到 Modbus 地址，字段如下：

- `point_id`：点位 ID
- `function_code`：Modbus 功能码
- `register_address`：起始地址
- `register_count`：寄存器数量
- `data_type`：原始数据类型
- `scale`：比例系数
- `offset`：偏移量

示例：

```csv
point_id,function_code,register_address,register_count,data_type,scale,offset
bess-start-stop,5,100,1,bool,1.0,0.0
bess-target-power,6,200,2,int32,1.0,0.0
bess-target-soc,6,202,1,uint16,0.1,0.0
```

其中：

- `FC=5`：单线圈写，适合开关量遥控
- `FC=6`：单寄存器写
- `FC=16`：多寄存器写
- `FC=1/2/3/4`：读功能码，分别对应 Coil、DI、Holding Register、Input Register

## 3. 遥信遥测是如何采集的

## 3.1 启动时的设备和点位装配

在 `collector_main.cpp` 中，程序会把配置文件里的设备和点位装配成内存对象：

- 只处理 `protocol != modbus-tcp` 之外的设备
- 每个点位会保留类别、类型、单位、是否可写以及 Modbus 映射

随后程序会创建共享内存 RtDb，并把点位注册进去：

- 遥信点注册为 `point_category = 1`
- 遥测点注册为 `point_category = 0`
- 遥控点注册为 `point_category = 2`
- 遥调点注册为 `point_category = 3`

对于可写点，如果其 `function_code` 是 `5`、`6` 或 `16`，还会额外创建命令槽，供后续下发使用。

## 3.2 轮询线程模型

`PollingService` 为每个设备启动一个线程，每个线程循环执行：

1. 调用 `DevicePollTask::poll_once()`
2. 按目标周期休眠
3. 重复执行

当前实现是“每设备一线程”的模型，适合设备数不太多的场景，逻辑清晰，也方便定位单台设备故障。

## 3.3 点位分组读取

轮询时不会逐点单独发请求，而是先按功能码分组，再按地址连续性合并，减少 Modbus 报文次数。

具体规则在 `DevicePollTask::build_register_groups()` 中：

- 先按功能码分组
- 再按地址排序
- 如果后一个点位地址距离当前分组末尾不超过 5 个寄存器，则合并到同一读组

这样可以把多个相邻点位合成一次 `read_holding_registers()` 或 `read_coils()` 调用，提高采集效率。

## 3.4 写点在采集时如何回读

当前实现中，如果点位本身是写功能码，也会在采集阶段被自动转换成对应的读功能码进行回读：

- `FC 5 -> FC 1`
- `FC 6 -> FC 3`
- `FC 16 -> FC 3`

也就是说：

- 遥控点通常通过 `Read Coils` 回读状态
- 遥调点通常通过 `Read Holding Registers` 回读设定值

这让“控制点当前值”也能被当作普通实时数据持续写入 RtDb。

## 3.5 数据解析与工程量换算

每组 Modbus 读结果返回后，会按点位映射逐点解析：

- 功能码 `1/2`：按 bit 解析
- 功能码 `3/4`：按寄存器解析

解析成功后会应用：

```text
工程值 = 原始值 * scale + offset
```

例如：

- `bess-target-soc` 配置为 `uint16 + scale=0.1`
- 如果设备原始寄存器值为 `500`
- 写入 RtDb 的工程值就是 `50.0`

## 3.6 遥信与遥测在 RtDb 中的存储方式

采集成功后，点位会被写入共享内存 RtDb：

- 遥信使用 `write_teleindication()`，存储为 `uint16 state_code`
- 遥测、遥控、遥调统一使用 `write_telemetry()`，存储为 `double value`

同时会写入：

- `timestamp`
- `quality`
- `valid`

如果某个读组失败，则该组下所有点位会被标记为：

- `quality = Bad`
- `valid = false`

注意，这里是“按分组失败回退”，不是整台设备所有点全部置坏。

## 4. 遥控遥调是如何下发的

## 4.1 哪些点可以下发

一个点位要能被下发，必须同时满足：

1. 点位定义中 `writable = true`
2. 有 `modbus_mapping`
3. `function_code` 为 `5`、`6` 或 `16`

如果只有 `writable = true`，但映射功能码仍然是读码，比如 `3`，那么它不会被注册为命令点，也无法下发。

这点在当前示例配置里尤其要注意：

- `teleindication.csv` 中 `bess-run-mode` 虽然写了 `writable=true`
- 但 `modbus_mapping.csv` 中它仍是 `FC=3`
- 所以当前实现不会把它当成真正可写命令点

当前样例中真正可下发的点包括：

- `bess-start-stop`
- `bess-target-power`
- `bess-target-soc`

## 4.2 命令下发流程

控制链路分为两段：

### 第一段：外部进程提交命令到 RtDb

外部系统并不直接操作 Modbus 客户端，而是先向共享内存中的命令槽写入命令：

- 点位 ID
- 目标值 `desired_value`
- 状态置为 `Pending`

标准 C++ 接口是：

```cpp
db->submit_command(point_id, desired_value);
```

Python 侧在 `src/web/shm_reader.py` 中也提供了同样用途的方法：

```python
reader.submit_command("bess-target-power", 1200)
```

### 第二段：ControlService 消费命令并执行写入

`ControlService` 会为每个存在可写点的设备创建一个控制线程，周期默认 100ms。

线程循环逻辑：

1. 调用 `read_pending_command()` 扫描命令槽
2. 找到属于本设备的点位
3. 检查该点是否存在 Modbus 映射
4. 根据功能码执行对应写操作
5. 回读确认
6. 调用 `complete_command()` 回写结果

## 4.3 遥控与遥调如何映射到 Modbus 写操作

`DeviceControlTask::execute_write()` 中的规则如下：

- `FC=5`：调用 `write_single_coil()`，适合开关量遥控
- `FC=6`：优先按单寄存器写；如果编码后需要多个寄存器，则退化为多寄存器写
- `FC=16`：调用 `write_multiple_registers()`，适合多寄存器遥调

写入前会先把工程值编码成 Modbus 原始值：

- 布尔遥控使用 `encode_coil()`
- 数值遥调使用 `encode_register()`

因此，外部系统下发时应传“工程值”，而不是原始寄存器值。

例如：

- `bess-start-stop = 1` 表示下发启动
- `bess-target-power = 5000` 表示下发目标功率 5000W
- `bess-target-soc = 80` 表示下发目标 SOC 为 80%，内部会结合 `scale=0.1` 编码成设备寄存器值

## 4.4 下发后的回读确认

命令写成功后，系统还会立即做一次回读校验：

- `FC=5` 写线圈后，用 `read_coils()` 回读
- `FC=6/16` 写寄存器后，用 `read_holding_registers()` 回读

回读值会重新按映射解析并做工程量换算，然后写入命令结果：

- `status = Success`
- `result_value = 回读后的工程值`

如果写失败，则会写入：

- `status = Failed`
- `error_code = 4`

另外还有几类常见失败码：

- `1`：点位不属于当前设备或未找到
- `2`：点位没有 Modbus 映射
- `3`：点位映射功能码不是写功能码

## 5. 共享内存中的命令状态

RtDb 命令槽支持以下状态：

- `Pending`
- `Executing`
- `Success`
- `Failed`
- `Idle`

外部系统可以通过以下接口查询命令结果：

```cpp
db->read_command_status(point_id);
```

或者通过 Python：

```python
reader.read_command_status("bess-target-power")
```

返回内容包括：

- `desired_value`
- `result_value`
- `submit_time`
- `complete_time`
- `status`
- `error_code`

## 6. 通过 Web 接口下发遥控遥调

当前仓库已经提供了一个 FastAPI 服务 `src/web/admin_server.py`（`src/web/server.py` 为兼容转发脚本），它基于共享内存提供 HTTP 接口。

### 6.1 获取实时快照

```http
GET /api/snapshot
```

该接口会返回当前共享内存中的遥测、遥信、遥控、遥调点值快照。

### 6.2 下发命令

```http
POST /api/command
Content-Type: application/json

{
  "point_id": "bess-target-power",
  "desired_value": 1200
}
```

返回示例：

```json
{
  "status": "submitted",
  "point_id": "bess-target-power",
  "desired_value": 1200
}
```

### 6.3 查询命令状态

```http
GET /api/command/bess-target-power
```

返回示例：

```json
{
  "point_id": "bess-target-power",
  "desired_value": 1200.0,
  "result_value": 1200.0,
  "submit_time": 1710000000000,
  "complete_time": 1710000000200,
  "status": "Success",
  "status_code": 2,
  "error_code": 0
}
```

## 7. 推荐操作流程

## 7.1 采集验证

建议按下面顺序验证采集链路：

1. 检查 `device.csv` 中设备协议是否为 `modbus-tcp`
2. 检查点位是否定义在 `telemetry.csv` / `teleindication.csv`
3. 检查 `modbus_mapping.csv` 中地址、功能码、寄存器数量是否正确
4. 启动 `openems-modbus-collector`
5. 启动 `openems-viewer` 或访问 `/api/snapshot`
6. 确认点值、质量位、时间戳是否正常变化

## 7.2 控制验证

建议按下面顺序验证下发链路：

1. 检查点位是否 `writable=true`
2. 检查映射功能码是否为 `5/6/16`
3. 确认 `collector` 启动日志里已经创建命令槽
4. 通过 `/api/command` 或 `submit_command()` 下发命令
5. 通过 `/api/command/{point_id}` 查询状态
6. 再通过 `/api/snapshot` 或 `viewer` 检查目标点回读值是否更新

## 8. 当前实现的几个注意点

- 轮询线程当前统一使用 `default_poll_interval_ms`，并没有在 `PollingService` 中按设备单独使用 `device.poll_interval_ms`。
- 控制线程通过扫描共享内存命令槽获取命令，属于轻量级实现，适合当前项目阶段。
- Python 侧 `shm_reader.py` 是直接写共享内存命令槽，而不是调用 C++ `RtDb::submit_command()`；两者语义基本一致，但后续若要增强并发安全性，建议统一走同一套接口封装。
- 遥控点和遥调点在 RtDb 中与遥测点共用 `write_telemetry()` 存储，因此外部查看时看到的是工程值 `double`。

## 9. 一句话总结

`openems-modbus-collector` 的核心模式是：

- 用 CSV 定义点表和 Modbus 映射
- 用轮询线程采集遥信遥测并写入共享内存
- 用命令槽承接遥控遥调请求
- 用控制线程把命令转换成 Modbus 写操作并回读确认

如果后续你希望，我可以继续把这份文档扩展成：

- 带时序图的版本
- 带“新增一个遥控点/遥调点的完整配置步骤”的版本
- 直接挂到 `README.md` 的简版操作手册
