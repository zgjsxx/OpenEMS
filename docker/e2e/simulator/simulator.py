import csv
import io
import json
import os
import signal
import struct
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from pymodbus.datastore import ModbusSequentialDataBlock, ModbusServerContext, ModbusSlaveContext
from pymodbus.server.sync import StartTcpServer


MODBUS_PORT = int(os.environ.get("SIM_MODBUS_PORT", "1502"))
HTTP_PORT = int(os.environ.get("SIM_HTTP_PORT", "18080"))
STEP_SECONDS = 0.5
RAMP_W_PER_S = float(os.environ.get("SIM_BESS_RAMP_W_PER_S", "20000"))
DEFAULT_BESS1_CAPACITY_KWH = float(os.environ.get("SIM_BESS1_CAPACITY_KWH", "80"))
DEFAULT_BESS2_CAPACITY_KWH = float(os.environ.get("SIM_BESS2_CAPACITY_KWH", "120"))
DEFAULT_BESS1_MAX_CHARGE_POWER_W = float(os.environ.get("SIM_BESS1_MAX_CHARGE_POWER_W", "60000"))
DEFAULT_BESS1_MAX_DISCHARGE_POWER_W = float(os.environ.get("SIM_BESS1_MAX_DISCHARGE_POWER_W", "60000"))
DEFAULT_BESS2_MAX_CHARGE_POWER_W = float(os.environ.get("SIM_BESS2_MAX_CHARGE_POWER_W", "100000"))
DEFAULT_BESS2_MAX_DISCHARGE_POWER_W = float(os.environ.get("SIM_BESS2_MAX_DISCHARGE_POWER_W", "100000"))
DEFAULT_PV1_RATED_POWER_W = float(os.environ.get("SIM_PV1_RATED_POWER_W", "80000"))
DEFAULT_PV2_RATED_POWER_W = float(os.environ.get("SIM_PV2_RATED_POWER_W", "120000"))

PV1_UNIT = 1
BESS1_UNIT = 2
GRID_UNIT = 3
PV2_UNIT = 4
BESS2_UNIT = 5

PV_DEVICES = [
    {"key": "pv", "label": "PV #1", "unit": PV1_UNIT, "rated_power_w": DEFAULT_PV1_RATED_POWER_W},
    {"key": "pv2", "label": "PV #2", "unit": PV2_UNIT, "rated_power_w": DEFAULT_PV2_RATED_POWER_W},
]
BESS_DEVICES = [
    {
        "key": "bess",
        "label": "BESS #1",
        "unit": BESS1_UNIT,
        "capacity_kwh": DEFAULT_BESS1_CAPACITY_KWH,
        "max_charge_power_w": DEFAULT_BESS1_MAX_CHARGE_POWER_W,
        "max_discharge_power_w": DEFAULT_BESS1_MAX_DISCHARGE_POWER_W,
    },
    {
        "key": "bess2",
        "label": "BESS #2",
        "unit": BESS2_UNIT,
        "capacity_kwh": DEFAULT_BESS2_CAPACITY_KWH,
        "max_charge_power_w": DEFAULT_BESS2_MAX_CHARGE_POWER_W,
        "max_discharge_power_w": DEFAULT_BESS2_MAX_DISCHARGE_POWER_W,
    },
]


def _int32_words(value: int) -> list[int]:
    packed = struct.pack(">i", int(value))
    return [int.from_bytes(packed[0:2], "big"), int.from_bytes(packed[2:4], "big")]


def _float32_words(value: float) -> list[int]:
    packed = struct.pack(">f", float(value))
    return [int.from_bytes(packed[0:2], "big"), int.from_bytes(packed[2:4], "big")]


def _words_to_int32(words: list[int]) -> int:
    raw = words[0].to_bytes(2, "big") + words[1].to_bytes(2, "big")
    return struct.unpack(">i", raw)[0]


def _make_default_state() -> dict:
    state = {
        "load_power_w": 10000,
        "grid_frequency_hz": 50.0,
        "grid_voltage_v": 400.0,
        "grid_on_off_status": 1,
        "grid_switch_position": 1,
    }
    for pv in PV_DEVICES:
        key = pv["key"]
        state[f"{key}_rated_power_w"] = pv["rated_power_w"]
        state[f"{key}_available_power_w"] = 18000
        state[f"{key}_power_w"] = 18000
        state[f"{key}_voltage_v"] = 400.0
        state[f"{key}_current_a"] = 45.0
        state[f"{key}_running_status"] = 1
        state[f"{key}_target_power_w"] = 0
        state[f"{key}_target_power_limit_pct"] = 100.0
    for bess in BESS_DEVICES:
        key = bess["key"]
        state[f"{key}_capacity_kwh"] = bess["capacity_kwh"]
        state[f"{key}_max_charge_power_w"] = bess["max_charge_power_w"]
        state[f"{key}_max_discharge_power_w"] = bess["max_discharge_power_w"]
        state[f"{key}_soc_pct"] = 50.0
        state[f"{key}_active_power_w"] = 0
        state[f"{key}_target_power_w"] = 0
        state[f"{key}_target_soc_pct"] = 50.0
        state[f"{key}_grid_state"] = 0
        state[f"{key}_run_mode"] = 1
        state[f"{key}_started"] = True
    return state


DEFAULT_STATE = _make_default_state()
state_lock = threading.Lock()
state = dict(DEFAULT_STATE)


FIELD_GROUPS = [
    {
        "title": "站级扰动",
        "fields": [
            "load_power_w",
            "grid_frequency_hz",
            "grid_voltage_v",
            "grid_on_off_status",
            "grid_switch_position",
        ],
    },
    {
        "title": "PV Inverter #1",
        "fields": [
            "pv_rated_power_w",
            "pv_available_power_w",
            "pv_power_w",
            "pv_voltage_v",
            "pv_current_a",
            "pv_running_status",
            "pv_target_power_w",
            "pv_target_power_limit_pct",
        ],
    },
    {
        "title": "PV Inverter #2",
        "fields": [
            "pv2_rated_power_w",
            "pv2_available_power_w",
            "pv2_power_w",
            "pv2_voltage_v",
            "pv2_current_a",
            "pv2_running_status",
            "pv2_target_power_w",
            "pv2_target_power_limit_pct",
        ],
    },
    {
        "title": "BESS #1",
        "fields": [
            "bess_capacity_kwh",
            "bess_max_charge_power_w",
            "bess_max_discharge_power_w",
            "bess_soc_pct",
            "bess_active_power_w",
            "bess_target_power_w",
            "bess_target_soc_pct",
            "bess_grid_state",
            "bess_run_mode",
            "bess_started",
        ],
    },
    {
        "title": "BESS #2",
        "fields": [
            "bess2_capacity_kwh",
            "bess2_max_charge_power_w",
            "bess2_max_discharge_power_w",
            "bess2_soc_pct",
            "bess2_active_power_w",
            "bess2_target_power_w",
            "bess2_target_soc_pct",
            "bess2_grid_state",
            "bess2_run_mode",
            "bess2_started",
        ],
    },
]

BOOLEAN_FIELDS = {"bess_started", "bess2_started"}
CSV_FIELDS = [
    "scenario_name",
    "note",
    "load_power_w",
    "pv_rated_power_w",
    "pv_available_power_w",
    "pv2_rated_power_w",
    "pv2_available_power_w",
    "bess_capacity_kwh",
    "bess_max_charge_power_w",
    "bess_max_discharge_power_w",
    "bess_soc_pct",
    "bess2_capacity_kwh",
    "bess2_max_charge_power_w",
    "bess2_max_discharge_power_w",
    "bess2_soc_pct",
    "bess_started",
    "bess2_started",
    "bess_run_mode",
    "bess2_run_mode",
    "bess_grid_state",
    "bess2_grid_state",
]

SIMULATOR_PAGE = """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>OpenEMS 多设备策略模拟器</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #0d1117;
      --panel: #161b22;
      --line: #2d3748;
      --text: #e6edf3;
      --muted: #8b98a5;
      --accent: #4fc3f7;
      --green: #22c55e;
      --warn: #f59e0b;
    }
    * { box-sizing: border-box; }
    body { margin: 0; font-family: "Segoe UI", "Microsoft YaHei", sans-serif; background: var(--bg); color: var(--text); }
    .page { max-width: 1540px; margin: 0 auto; padding: 24px; }
    h1, h2, h3, p { margin: 0; }
    .subtitle { color: var(--muted); margin: 8px 0 20px; line-height: 1.7; }
    .toolbar { display: flex; flex-wrap: wrap; gap: 12px; margin-bottom: 20px; }
    button, .file-button {
      appearance: none; border: 1px solid #35506b; border-radius: 10px;
      padding: 10px 16px; background: #1f2a37; color: var(--text); cursor: pointer; font-weight: 600;
    }
    button.primary { background: linear-gradient(135deg, #32b6ff, #4fc3f7); color: #081018; border-color: transparent; }
    button.warn { background: #4a2a0a; border-color: #8a4b0f; color: #ffd8a8; }
    input[type="file"] { display: none; }
    .status { min-height: 24px; color: var(--muted); margin-bottom: 16px; }
    .status.ok { color: var(--green); }
    .status.err { color: #f87171; }
    .layout { display: grid; grid-template-columns: 1.2fr 1fr; gap: 20px; }
    .panel { background: var(--panel); border: 1px solid var(--line); border-radius: 16px; padding: 20px; }
    .section-title { font-size: 18px; font-weight: 700; margin-bottom: 14px; }
    .group-title { font-size: 15px; font-weight: 700; margin: 16px 0 10px; color: var(--accent); }
    .field-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; }
    .field { display: flex; flex-direction: column; gap: 6px; }
    .field label { color: var(--muted); font-size: 13px; }
    .field input { width: 100%; background: #0f1720; border: 1px solid #304154; color: var(--text); border-radius: 10px; padding: 10px 12px; }
    .checkbox-row { display: flex; align-items: center; gap: 10px; padding-top: 24px; }
    .checkbox-row input { width: 18px; height: 18px; accent-color: var(--accent); }
    table { width: 100%; border-collapse: collapse; font-size: 13px; }
    th, td { border-bottom: 1px solid var(--line); padding: 10px 8px; text-align: left; vertical-align: top; }
    th { color: var(--muted); font-weight: 600; }
    .note { color: var(--muted); line-height: 1.7; font-size: 13px; }
    .pill { display: inline-block; border: 1px solid #35506b; border-radius: 999px; padding: 4px 10px; color: var(--accent); font-size: 12px; margin: 0 8px 8px 0; }
    .csv-wrap { max-height: 620px; overflow: auto; }
    @media (max-width: 1180px) {
      .layout { grid-template-columns: 1fr; }
      .field-grid { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <div class="page">
    <h1>OpenEMS 多设备策略模拟器</h1>
    <p class="subtitle">用于手动联调双光伏、双储能、单关口表场景。你可以直接改表单，也可以导入 CSV 场景，一键把某一行写入模拟器。</p>

    <div class="toolbar">
      <button id="reload-btn">刷新当前状态</button>
      <button id="apply-btn" class="primary">应用表单到模拟器</button>
      <button id="reset-btn" class="warn">重置为默认状态</button>
      <label class="file-button">
        导入 CSV 场景
        <input id="csv-file" type="file" accept=".csv,text/csv">
      </label>
      <a href="/sample.csv" style="align-self:center;color:var(--accent);text-decoration:none;">下载示例 CSV</a>
    </div>
    <div id="status" class="status">页面已加载。</div>

    <div class="layout">
      <section class="panel">
        <h2 class="section-title">模拟器状态</h2>
        <div id="state-form"></div>
      </section>
      <section class="panel">
        <h2 class="section-title">CSV 场景表</h2>
        <div id="csv-summary" class="note">尚未导入 CSV。建议每一行至少给出两台 PV 可发功率、两台 BESS SOC 和负荷。</div>
        <div style="margin-top: 12px;">
          <span class="pill">scenario_name</span>
          <span class="pill">note</span>
          <span class="pill">pv_available_power_w</span>
          <span class="pill">pv2_available_power_w</span>
          <span class="pill">bess_soc_pct</span>
          <span class="pill">bess2_soc_pct</span>
          <span class="pill">load_power_w</span>
          <span class="pill">bess_started</span>
          <span class="pill">bess2_started</span>
        </div>
        <div class="csv-wrap">
          <table id="csv-table">
            <thead>
              <tr>
                <th>操作</th>
                <th>场景名</th>
                <th>说明</th>
                <th>关键值</th>
              </tr>
            </thead>
            <tbody>
              <tr><td colspan="4" style="color:var(--muted);">导入 CSV 后可逐行应用场景。</td></tr>
            </tbody>
          </table>
        </div>
      </section>
    </div>
  </div>

  <script>
    const BOOLEAN_FIELDS = new Set(["bess_started", "bess2_started"]);
    const FIELD_GROUPS = __FIELD_GROUPS__;
    const stateForm = document.getElementById("state-form");
    const statusEl = document.getElementById("status");
    const csvInput = document.getElementById("csv-file");
    const csvSummary = document.getElementById("csv-summary");
    const csvTableBody = document.querySelector("#csv-table tbody");

    function setStatus(message, kind = "") {
      statusEl.textContent = message;
      statusEl.className = "status" + (kind ? " " + kind : "");
    }

    function createField(key, value) {
      const wrapper = document.createElement("div");
      wrapper.className = BOOLEAN_FIELDS.has(key) ? "field checkbox-row" : "field";

      const label = document.createElement("label");
      label.textContent = key;
      label.htmlFor = "field-" + key;

      const input = document.createElement("input");
      input.id = "field-" + key;
      input.dataset.key = key;

      if (BOOLEAN_FIELDS.has(key)) {
        input.type = "checkbox";
        input.checked = Boolean(value);
        wrapper.appendChild(input);
        wrapper.appendChild(label);
      } else {
        input.type = "number";
        input.step = "any";
        input.value = value ?? 0;
        wrapper.appendChild(label);
        wrapper.appendChild(input);
      }
      return wrapper;
    }

    function renderStateForm(data) {
      stateForm.innerHTML = "";
      FIELD_GROUPS.forEach(group => {
        const title = document.createElement("div");
        title.className = "group-title";
        title.textContent = group.title;
        stateForm.appendChild(title);

        const grid = document.createElement("div");
        grid.className = "field-grid";
        group.fields.forEach(key => {
          grid.appendChild(createField(key, data[key]));
        });
        stateForm.appendChild(grid);
      });
    }

    function collectPayload() {
      const payload = {};
      stateForm.querySelectorAll("input").forEach(el => {
        const key = el.dataset.key;
        if (!key) return;
        payload[key] = BOOLEAN_FIELDS.has(key) ? el.checked : Number(el.value);
      });
      return payload;
    }

    async function fetchJson(url, options = {}) {
      const response = await fetch(url, options);
      if (!response.ok) {
        throw new Error(await response.text() || ("HTTP " + response.status));
      }
      return response.json();
    }

    async function loadState() {
      const data = await fetchJson("/state");
      renderStateForm(data);
      setStatus("已刷新当前状态。", "ok");
    }

    async function applyPayload(payload, label = "手动修改") {
      const data = await fetchJson("/state", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(payload),
      });
      renderStateForm(data);
      setStatus(label + " 已应用。", "ok");
    }

    async function resetState() {
      const data = await fetchJson("/reset", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: "{}",
      });
      renderStateForm(data);
      setStatus("已重置为默认状态。", "ok");
    }

    function parseCsv(text) {
      const rows = [];
      let row = [];
      let cell = "";
      let inQuotes = false;
      for (let i = 0; i < text.length; i += 1) {
        const ch = text[i];
        if (inQuotes) {
          if (ch === '"') {
            if (text[i + 1] === '"') {
              cell += '"';
              i += 1;
            } else {
              inQuotes = false;
            }
          } else {
            cell += ch;
          }
        } else if (ch === '"') {
          inQuotes = true;
        } else if (ch === ",") {
          row.push(cell);
          cell = "";
        } else if (ch === "\\n") {
          row.push(cell);
          rows.push(row);
          row = [];
          cell = "";
        } else if (ch !== "\\r") {
          cell += ch;
        }
      }
      row.push(cell);
      rows.push(row);
      return rows.filter(r => r.some(col => String(col).trim() !== ""));
    }

    function normalizeValue(key, value) {
      if (value == null || String(value).trim() === "") return undefined;
      const text = String(value).trim();
      if (BOOLEAN_FIELDS.has(key)) {
        return ["1", "true", "yes", "on"].includes(text.toLowerCase());
      }
      const num = Number(text);
      return Number.isFinite(num) ? num : undefined;
    }

    function renderCsvRows(rows) {
      csvTableBody.innerHTML = "";
      if (!rows.length) {
        csvTableBody.innerHTML = '<tr><td colspan="4" style="color:var(--muted);">CSV 里没有有效场景。</td></tr>';
        return;
      }

      rows.forEach((row, index) => {
        const tr = document.createElement("tr");
        const actionTd = document.createElement("td");
        const nameTd = document.createElement("td");
        const noteTd = document.createElement("td");
        const valueTd = document.createElement("td");

        const btn = document.createElement("button");
        btn.textContent = "应用";
        btn.className = "primary";
        btn.addEventListener("click", async () => {
          const payload = {};
          FIELD_GROUPS.flatMap(group => group.fields).forEach(key => {
            const normalized = normalizeValue(key, row[key]);
            if (normalized !== undefined) payload[key] = normalized;
          });
          await applyPayload(payload, "CSV 场景 #" + (index + 1));
        });
        actionTd.appendChild(btn);

        nameTd.textContent = row.scenario_name || ("场景 " + (index + 1));
        noteTd.textContent = row.note || "-";
        valueTd.textContent =
          "PV1=" + (row.pv_available_power_w ?? "-") +
          ", PV2=" + (row.pv2_available_power_w ?? "-") +
          ", BESS1 SOC=" + (row.bess_soc_pct ?? "-") +
          ", BESS2 SOC=" + (row.bess2_soc_pct ?? "-") +
          ", Load=" + (row.load_power_w ?? "-");

        tr.appendChild(actionTd);
        tr.appendChild(nameTd);
        tr.appendChild(noteTd);
        tr.appendChild(valueTd);
        csvTableBody.appendChild(tr);
      });
    }

    csvInput.addEventListener("change", async (event) => {
      const file = event.target.files && event.target.files[0];
      if (!file) return;
      const text = await file.text();
      const rows = parseCsv(text);
      if (!rows.length) {
        renderCsvRows([]);
        csvSummary.textContent = "CSV 为空或没有有效行。";
        setStatus("CSV 导入失败：没有有效数据。", "err");
        return;
      }
      const headers = rows[0].map(item => String(item).trim());
      const bodyRows = rows.slice(1).map(cols => {
        const entry = {};
        headers.forEach((header, index) => {
          entry[header] = cols[index] ?? "";
        });
        return entry;
      }).filter(row => Object.values(row).some(value => String(value).trim() !== ""));
      renderCsvRows(bodyRows);
      csvSummary.textContent = "已导入 " + bodyRows.length + " 条多设备场景。点击“应用”可把该行状态写入模拟器。";
      setStatus("CSV 导入成功。", "ok");
    });

    document.getElementById("reload-btn").addEventListener("click", () => loadState().catch(err => setStatus("刷新失败: " + err.message, "err")));
    document.getElementById("apply-btn").addEventListener("click", () => applyPayload(collectPayload()).catch(err => setStatus("应用失败: " + err.message, "err")));
    document.getElementById("reset-btn").addEventListener("click", () => resetState().catch(err => setStatus("重置失败: " + err.message, "err")));
    loadState().catch(err => setStatus("初始化失败: " + err.message, "err"));
  </script>
</body>
</html>
"""

FIELD_GROUPS_JSON = json.dumps(FIELD_GROUPS, ensure_ascii=False)
SIMULATOR_PAGE = SIMULATOR_PAGE.replace("__FIELD_GROUPS__", FIELD_GROUPS_JSON)

SAMPLE_CSV = """scenario_name,note,load_power_w,pv_rated_power_w,pv_available_power_w,pv2_rated_power_w,pv2_available_power_w,bess_capacity_kwh,bess_max_charge_power_w,bess_max_discharge_power_w,bess_soc_pct,bess2_capacity_kwh,bess2_max_charge_power_w,bess2_max_discharge_power_w,bess2_soc_pct,bess_started,bess2_started,bess_run_mode,bess2_run_mode,bess_grid_state,bess2_grid_state
双储能防逆流,SOC 正常，两台储能共同吸收逆流,10000,80000,18000,120000,18000,80,60000,60000,50,120,100000,100000,55,true,true,1,1,0,0
双储能高SOC转限光,SOC 都过高，改由双光伏限发,10000,80000,18000,120000,18000,80,60000,60000,90,120,100000,100000,92,true,true,1,1,0,0
单储能参与,BESS #2 停机，仅 BESS #1 参与,10000,80000,18000,120000,18000,80,60000,60000,50,120,100000,100000,50,true,false,1,0,0,0
单光伏停机,PV #2 停机，观察剩余设备接管,10000,80000,18000,120000,0,80,60000,60000,50,120,100000,100000,50,true,true,1,1,0,0
"""


def _make_slave_context() -> ModbusSlaveContext:
    return ModbusSlaveContext(
        di=ModbusSequentialDataBlock(0, [0] * 512),
        co=ModbusSequentialDataBlock(0, [0] * 512),
        hr=ModbusSequentialDataBlock(0, [0] * 512),
        ir=ModbusSequentialDataBlock(0, [0] * 512),
        zero_mode=True,
    )


server_context = ModbusServerContext(
    slaves={
        PV1_UNIT: _make_slave_context(),
        BESS1_UNIT: _make_slave_context(),
        GRID_UNIT: _make_slave_context(),
        PV2_UNIT: _make_slave_context(),
        BESS2_UNIT: _make_slave_context(),
    },
    single=False,
)


def _slave(unit: int) -> ModbusSlaveContext:
    return server_context[unit]


def _pv_state_keys(base: str) -> dict:
    return {
        "rated": f"{base}_rated_power_w",
        "available": f"{base}_available_power_w",
        "power": f"{base}_power_w",
        "voltage": f"{base}_voltage_v",
        "current": f"{base}_current_a",
        "running": f"{base}_running_status",
        "target_power": f"{base}_target_power_w",
        "target_limit": f"{base}_target_power_limit_pct",
    }


def _bess_state_keys(base: str) -> dict:
    return {
        "capacity": f"{base}_capacity_kwh",
        "soc": f"{base}_soc_pct",
        "power": f"{base}_active_power_w",
        "target_power": f"{base}_target_power_w",
        "target_soc": f"{base}_target_soc_pct",
        "grid_state": f"{base}_grid_state",
        "run_mode": f"{base}_run_mode",
        "started": f"{base}_started",
    }


def _read_targets_from_registers() -> None:
    with state_lock:
        for pv in PV_DEVICES:
            keys = _pv_state_keys(pv["key"])
            slave = _slave(pv["unit"])
            target_words = slave.getValues(3, 17, count=2)
            target_limit = slave.getValues(3, 20, count=1)
            state[keys["target_power"]] = _words_to_int32(target_words)
            state[keys["target_limit"]] = (target_limit[0] if target_limit else 1000) / 10.0

        for bess in BESS_DEVICES:
            keys = _bess_state_keys(bess["key"])
            slave = _slave(bess["unit"])
            target_words = slave.getValues(3, 200, count=2)
            target_soc = slave.getValues(3, 202, count=1)
            start_coil = slave.getValues(1, 100, count=1)
            state[keys["target_power"]] = _words_to_int32(target_words)
            state[keys["target_soc"]] = (target_soc[0] if target_soc else 500) / 10.0
            state[keys["started"]] = bool(start_coil[0]) if start_coil else True


def _sync_registers() -> None:
    with state_lock:
        load_power_w = int(state["load_power_w"])
        grid_frequency_hz = float(state["grid_frequency_hz"])
        grid_voltage_v = float(state["grid_voltage_v"])
        grid_on_off_status = int(state["grid_on_off_status"])
        grid_switch_position = int(state["grid_switch_position"])

        total_pv_power_w = 0
        total_bess_power_w = 0

        for pv in PV_DEVICES:
            keys = _pv_state_keys(pv["key"])
            available = int(state[keys["available"]])
            rated_power = max(0.0, float(state[keys["rated"]]))
            voltage = float(state[keys["voltage"]])
            running = int(state[keys["running"]])
            target_power = int(state[keys["target_power"]])
            target_limit_pct = max(0.0, min(100.0, float(state[keys["target_limit"]])))

            if running:
                limit_power = rated_power * target_limit_pct / 100.0
                actual_power = int(round(min(float(available), limit_power)))
            else:
                actual_power = 0
            current = (actual_power / max(voltage, 1.0)) if voltage > 0 else 0.0
            total_pv_power_w += actual_power

            pv_slave = _slave(pv["unit"])
            pv_slave.setValues(3, 0, _int32_words(actual_power))
            pv_slave.setValues(3, 2, [int(round(voltage * 10.0))])
            pv_slave.setValues(3, 3, [int(round(current * 100.0))])
            pv_slave.setValues(3, 4, [running])
            pv_slave.setValues(3, 15, [1 if running else 0])
            pv_slave.setValues(3, 17, _int32_words(target_power))
            pv_slave.setValues(3, 20, [int(round(target_limit_pct * 10.0))])

            state[keys["power"]] = actual_power
            state[keys["current"]] = round(current, 3)

        for bess in BESS_DEVICES:
            keys = _bess_state_keys(bess["key"])
            soc = float(state[keys["soc"]])
            active_power = int(state[keys["power"]])
            target_power = int(state[keys["target_power"]])
            target_soc = float(state[keys["target_soc"]])
            grid_state = int(state[keys["grid_state"]])
            run_mode = int(state[keys["run_mode"]])
            started = bool(state[keys["started"]])

            total_bess_power_w += active_power

            slave = _slave(bess["unit"])
            slave.setValues(3, 0, [int(round(soc * 10.0))])
            slave.setValues(3, 1, _int32_words(active_power))
            slave.setValues(3, 5, [grid_state])
            slave.setValues(3, 6, [run_mode])
            slave.setValues(1, 100, [1 if started else 0])
            slave.setValues(3, 200, _int32_words(target_power))
            slave.setValues(3, 202, [int(round(target_soc * 10.0))])

    grid_power_kw = (load_power_w - total_pv_power_w - total_bess_power_w) / 1000.0
    grid_slave = _slave(GRID_UNIT)
    grid_slave.setValues(3, 0, _float32_words(grid_power_kw))
    grid_slave.setValues(3, 2, _float32_words(grid_frequency_hz))
    grid_slave.setValues(3, 4, _float32_words(grid_voltage_v))
    grid_slave.setValues(3, 6, [grid_on_off_status])
    grid_slave.setValues(3, 7, [grid_switch_position])

    with state_lock:
        state["grid_active_power_kw"] = grid_power_kw


def _reset_state() -> None:
    with state_lock:
        state.clear()
        state.update(DEFAULT_STATE)
    _sync_registers()


def _apply_state_patch(payload: dict) -> None:
    pv_aliases = {"pv_power_w": "pv_available_power_w", "pv2_power_w": "pv2_available_power_w"}
    with state_lock:
        for key, value in payload.items():
            if key in pv_aliases:
                state[pv_aliases[key]] = value
                continue
            if key not in state:
                continue
            state[key] = value

        for bess in BESS_DEVICES:
            keys = _bess_state_keys(bess["key"])
            if keys["target_power"] in payload or keys["target_soc"] in payload:
                slave = _slave(bess["unit"])
                slave.setValues(3, 200, _int32_words(int(state[keys["target_power"]])))
                slave.setValues(3, 202, [int(round(float(state[keys["target_soc"]]) * 10.0))])
            if keys["started"] in payload:
                _slave(bess["unit"]).setValues(1, 100, [1 if payload[keys["started"]] else 0])
    _sync_registers()


def _simulation_loop(stop_event: threading.Event) -> None:
    _reset_state()
    while not stop_event.is_set():
        _read_targets_from_registers()
        with state_lock:
            for bess in BESS_DEVICES:
                keys = _bess_state_keys(bess["key"])
                target = int(state[keys["target_power"]])
                actual = float(state[keys["power"]])
                started = bool(state[keys["started"]])
                run_mode = int(state[keys["run_mode"]])
                soc = float(state[keys["soc"]])
                capacity_wh = max(1.0, float(state[keys["capacity"]])) * 1000.0

                effective_target = target if started and run_mode != 0 else 0
                max_step = RAMP_W_PER_S * STEP_SECONDS
                if actual < effective_target:
                    actual = min(actual + max_step, effective_target)
                elif actual > effective_target:
                    actual = max(actual - max_step, effective_target)

                delta_soc = (-actual * STEP_SECONDS / 3600.0) / capacity_wh * 100.0
                soc = min(100.0, max(0.0, soc + delta_soc))

                state[keys["power"]] = int(round(actual))
                state[keys["soc"]] = round(soc, 3)
        _sync_registers()
        stop_event.wait(STEP_SECONDS)


class SimulatorHandler(BaseHTTPRequestHandler):
    def _send_html(self, content: str, status: int = 200) -> None:
        data = content.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_csv(self, content: str, status: int = 200) -> None:
        data = content.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/csv; charset=utf-8")
        self.send_header("Content-Disposition", 'attachment; filename="sample_strategy_scenarios.csv"')
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_json(self, payload: dict, status: int = 200) -> None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        if self.path in ("/", "/index.html"):
            self._send_html(SIMULATOR_PAGE)
            return
        if self.path == "/sample.csv":
            self._send_csv(SAMPLE_CSV)
            return
        if self.path != "/state":
            self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)
            return
        with state_lock:
            payload = dict(state)
        self._send_json(payload)

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            payload = json.loads(raw.decode("utf-8") or "{}")
        except Exception:
            self._send_json({"error": "invalid json"}, HTTPStatus.BAD_REQUEST)
            return

        if self.path == "/reset":
            _reset_state()
        elif self.path == "/state":
            _apply_state_patch(payload if isinstance(payload, dict) else {})
        else:
            self._send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)
            return

        with state_lock:
            current = dict(state)
        self._send_json(current)

    def log_message(self, fmt: str, *args) -> None:
        print("[sim-http] " + (fmt % args), flush=True)


def main() -> None:
    stop_event = threading.Event()

    def _shutdown(*_args) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    sim_thread = threading.Thread(target=_simulation_loop, args=(stop_event,), daemon=True)
    sim_thread.start()

    modbus_thread = threading.Thread(
        target=StartTcpServer,
        kwargs={"context": server_context, "address": ("0.0.0.0", MODBUS_PORT)},
        daemon=True,
    )
    modbus_thread.start()

    httpd = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), SimulatorHandler)
    httpd.timeout = 0.5
    print(f"[sim] HTTP server listening on 0.0.0.0:{HTTP_PORT}", flush=True)
    print(f"[sim] Modbus TCP server listening on 0.0.0.0:{MODBUS_PORT}", flush=True)
    try:
      while not stop_event.is_set():
        httpd.handle_request()
    finally:
      httpd.server_close()
      stop_event.set()
      sim_thread.join(timeout=2.0)


if __name__ == "__main__":
    main()
