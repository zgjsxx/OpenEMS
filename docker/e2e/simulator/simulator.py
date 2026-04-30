import json
import math
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
PV_UNIT = 1
BESS_UNIT = 2
GRID_UNIT = 3
CAPACITY_KWH = float(os.environ.get("SIM_BESS_CAPACITY_KWH", "100"))
RAMP_W_PER_S = float(os.environ.get("SIM_BESS_RAMP_W_PER_S", "20000"))
STEP_SECONDS = 0.5


def _int32_words(value: int) -> list[int]:
    packed = struct.pack(">i", int(value))
    return [int.from_bytes(packed[0:2], "big"), int.from_bytes(packed[2:4], "big")]


def _float32_words(value: float) -> list[int]:
    packed = struct.pack(">f", float(value))
    return [int.from_bytes(packed[0:2], "big"), int.from_bytes(packed[2:4], "big")]


def _words_to_int32(words: list[int]) -> int:
    raw = words[0].to_bytes(2, "big") + words[1].to_bytes(2, "big")
    return struct.unpack(">i", raw)[0]


DEFAULT_STATE = {
    "pv_power_w": 36000,
    "pv_voltage_v": 400.0,
    "pv_current_a": 90.0,
    "pv_running_status": 1,
    "pv_target_power_w": 23000,
    "pv_target_power_limit_pct": 100.0,
    "load_power_w": 10000,
    "bess_soc_pct": 50.0,
    "bess_active_power_w": 0,
    "bess_target_power_w": 0,
    "bess_target_soc_pct": 50.0,
    "bess_grid_state": 0,
    "bess_run_mode": 1,
    "bess_started": True,
    "grid_frequency_hz": 50.0,
    "grid_voltage_v": 400.0,
    "grid_on_off_status": 1,
    "grid_switch_position": 1,
}


state_lock = threading.Lock()
state = dict(DEFAULT_STATE)

SIMULATOR_PAGE = """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>OpenEMS E2E Simulator</title>
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
    body {
      margin: 0;
      font-family: "Segoe UI", "Microsoft YaHei", sans-serif;
      background: var(--bg);
      color: var(--text);
    }
    .page {
      padding: 24px;
      max-width: 1400px;
      margin: 0 auto;
    }
    h1, h2, h3, p { margin: 0; }
    .subtitle {
      color: var(--muted);
      margin-top: 8px;
      margin-bottom: 20px;
    }
    .grid {
      display: grid;
      grid-template-columns: 1.1fr 1fr;
      gap: 20px;
    }
    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 16px;
      padding: 20px;
    }
    .toolbar {
      display: flex;
      flex-wrap: wrap;
      gap: 12px;
      margin-bottom: 20px;
    }
    button, .file-button {
      appearance: none;
      border: 1px solid #35506b;
      border-radius: 10px;
      padding: 10px 16px;
      background: #1f2a37;
      color: var(--text);
      cursor: pointer;
      font-weight: 600;
    }
    button.primary {
      background: linear-gradient(135deg, #32b6ff, #4fc3f7);
      color: #081018;
      border-color: transparent;
    }
    button.warn {
      background: #4a2a0a;
      border-color: #8a4b0f;
      color: #ffd8a8;
    }
    input[type="file"] { display: none; }
    .status {
      margin-top: 12px;
      min-height: 24px;
      color: var(--muted);
    }
    .status.ok { color: var(--green); }
    .status.err { color: #f87171; }
    .section-title {
      margin-bottom: 14px;
      font-size: 18px;
      font-weight: 700;
    }
    .field-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 12px;
    }
    .field {
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .field label {
      color: var(--muted);
      font-size: 13px;
    }
    .field input, .field select {
      width: 100%;
      background: #0f1720;
      border: 1px solid #304154;
      color: var(--text);
      border-radius: 10px;
      padding: 10px 12px;
    }
    .field input[type="checkbox"] {
      width: 18px;
      height: 18px;
      accent-color: var(--accent);
    }
    .checkbox-row {
      display: flex;
      align-items: center;
      gap: 10px;
      padding-top: 24px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 13px;
    }
    th, td {
      border-bottom: 1px solid var(--line);
      padding: 10px 8px;
      text-align: left;
      vertical-align: top;
    }
    th {
      color: var(--muted);
      font-weight: 600;
    }
    .csv-note {
      color: var(--muted);
      font-size: 13px;
      line-height: 1.6;
      margin-top: 10px;
    }
    .pill {
      display: inline-block;
      border: 1px solid #35506b;
      border-radius: 999px;
      padding: 4px 10px;
      color: var(--accent);
      font-size: 12px;
      margin-right: 8px;
      margin-bottom: 8px;
    }
    @media (max-width: 1080px) {
      .grid { grid-template-columns: 1fr; }
      .field-grid { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <div class="page">
    <h1>OpenEMS E2E Simulator</h1>
    <p class="subtitle">用于策略联调的模拟器页面。支持查看当前状态、手动改值，以及导入 CSV 场景表进行手工测试。</p>

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

    <div class="grid">
      <section class="panel">
        <h2 class="section-title">模拟器状态</h2>
        <div class="field-grid" id="state-form"></div>
      </section>

      <section class="panel">
        <h2 class="section-title">CSV 场景表</h2>
        <div id="csv-summary" class="csv-note">尚未导入 CSV。CSV 首行必须是表头，字段名建议直接使用模拟器状态键名。</div>
        <div style="margin-top:16px;overflow:auto;max-height:580px;">
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
        <div class="csv-note">
          支持的常用字段：
          <div style="margin-top:10px;">
            <span class="pill">scenario_name</span>
            <span class="pill">note</span>
            <span class="pill">bess_soc_pct</span>
            <span class="pill">bess_active_power_w</span>
            <span class="pill">bess_target_power_w</span>
            <span class="pill">pv_power_w</span>
            <span class="pill">load_power_w</span>
            <span class="pill">bess_started</span>
            <span class="pill">bess_run_mode</span>
            <span class="pill">bess_grid_state</span>
          </div>
        </div>
      </section>
    </div>
  </div>

  <script>
    const BOOLEAN_FIELDS = new Set(["bess_started"]);
    const STATE_FIELDS = [
      "pv_power_w", "pv_voltage_v", "pv_current_a", "pv_running_status",
      "pv_target_power_w", "pv_target_power_limit_pct",
      "load_power_w",
      "bess_soc_pct", "bess_active_power_w", "bess_target_power_w",
      "bess_target_soc_pct", "bess_grid_state", "bess_run_mode", "bess_started",
      "grid_frequency_hz", "grid_voltage_v", "grid_on_off_status", "grid_switch_position"
    ];

    const stateForm = document.getElementById("state-form");
    const statusEl = document.getElementById("status");
    const csvInput = document.getElementById("csv-file");
    const csvSummary = document.getElementById("csv-summary");
    const csvTableBody = document.querySelector("#csv-table tbody");
    let importedRows = [];

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
      for (const key of STATE_FIELDS) {
        stateForm.appendChild(createField(key, data[key]));
      }
    }

    function collectFormPayload() {
      const payload = {};
      for (const el of stateForm.querySelectorAll("input")) {
        const key = el.dataset.key;
        if (!key) continue;
        if (BOOLEAN_FIELDS.has(key)) {
          payload[key] = el.checked;
        } else {
          payload[key] = Number(el.value);
        }
      }
      return payload;
    }

    async function fetchJson(url, options = {}) {
      const response = await fetch(url, options);
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || ("HTTP " + response.status));
      }
      const contentType = response.headers.get("content-type") || "";
      if (contentType.includes("application/json")) {
        return response.json();
      }
      return response.text();
    }

    async function loadState() {
      const data = await fetchJson("/state");
      renderStateForm(data);
      setStatus("已刷新当前状态。", "ok");
      return data;
    }

    async function applyPayload(payload, label = "手工修改") {
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
      setStatus("模拟器已重置为默认状态。", "ok");
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
        } else if (ch === "\\r") {
          // ignore
        } else {
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
        csvTableBody.innerHTML = '<tr><td colspan="4" style="color:var(--muted);">CSV 中没有有效场景。</td></tr>';
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
          for (const key of STATE_FIELDS) {
            const normalized = normalizeValue(key, row[key]);
            if (normalized !== undefined) payload[key] = normalized;
          }
          await applyPayload(payload, "CSV 场景 #" + (index + 1));
        });
        actionTd.appendChild(btn);

        nameTd.textContent = row.scenario_name || ("场景 " + (index + 1));
        noteTd.textContent = row.note || "-";
        valueTd.textContent =
          "SOC=" + (row.bess_soc_pct ?? "-") +
          ", PV=" + (row.pv_power_w ?? "-") +
          ", Load=" + (row.load_power_w ?? "-") +
          ", Target=" + (row.bess_target_power_w ?? "-");

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
        importedRows = [];
        renderCsvRows(importedRows);
        csvSummary.textContent = "CSV 为空或没有有效行。";
        setStatus("CSV 导入失败：没有有效数据。", "err");
        return;
      }

      const headers = rows[0].map(item => String(item).trim());
      importedRows = rows.slice(1).map(cols => {
        const entry = {};
        headers.forEach((header, index) => {
          entry[header] = cols[index] ?? "";
        });
        return entry;
      }).filter(row => Object.values(row).some(value => String(value).trim() !== ""));

      renderCsvRows(importedRows);
      csvSummary.textContent =
        "已导入 " + importedRows.length + " 条场景。点击“应用”可把该行数据写入模拟器。";
      setStatus("CSV 导入成功。", "ok");
    });

    document.getElementById("reload-btn").addEventListener("click", () => {
      loadState().catch(err => setStatus("刷新失败: " + err.message, "err"));
    });
    document.getElementById("apply-btn").addEventListener("click", () => {
      applyPayload(collectFormPayload()).catch(err => setStatus("应用失败: " + err.message, "err"));
    });
    document.getElementById("reset-btn").addEventListener("click", () => {
      resetState().catch(err => setStatus("重置失败: " + err.message, "err"));
    });

    loadState().catch(err => setStatus("初始化失败: " + err.message, "err"));
  </script>
</body>
</html>
"""

SAMPLE_CSV = """scenario_name,note,bess_soc_pct,bess_active_power_w,bess_target_power_w,pv_power_w,load_power_w,bess_started,bess_run_mode,bess_grid_state
防逆流-正常SOC,SOC 正常且存在反送电,50,0,0,36000,10000,true,1,0
SOC高位抑制,SOC 已高于上限，禁止继续充电,90,0,0,36000,10000,true,1,0
SOC低位场景,后续可用于验证低 SOC 保护,10,0,0,8000,26000,true,1,0
储能停机,用于验证运行状态抑制,50,0,0,36000,10000,false,0,0
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
        PV_UNIT: _make_slave_context(),
        BESS_UNIT: _make_slave_context(),
        GRID_UNIT: _make_slave_context(),
    },
    single=False,
)


def _slave(unit: int) -> ModbusSlaveContext:
    return server_context[unit]


def _read_bess_target_state() -> None:
    bess = _slave(BESS_UNIT)
    target_words = bess.getValues(3, 200, count=2)
    target_soc = bess.getValues(3, 202, count=1)
    start_coil = bess.getValues(1, 100, count=1)
    with state_lock:
        state["bess_target_power_w"] = _words_to_int32(target_words)
        state["bess_target_soc_pct"] = (target_soc[0] if target_soc else 500) / 10.0
        state["bess_started"] = bool(start_coil[0]) if start_coil else True


def _sync_registers() -> None:
    with state_lock:
        pv_power_w = int(state["pv_power_w"])
        pv_voltage_v = float(state["pv_voltage_v"])
        pv_current_a = float(state["pv_current_a"])
        pv_running_status = int(state["pv_running_status"])
        pv_target_power_w = int(state["pv_target_power_w"])
        pv_target_power_limit_pct = float(state["pv_target_power_limit_pct"])
        bess_soc_pct = float(state["bess_soc_pct"])
        bess_active_power_w = int(state["bess_active_power_w"])
        bess_target_power_w = int(state["bess_target_power_w"])
        bess_target_soc_pct = float(state["bess_target_soc_pct"])
        bess_grid_state = int(state["bess_grid_state"])
        bess_run_mode = int(state["bess_run_mode"])
        bess_started = bool(state["bess_started"])
        grid_frequency_hz = float(state["grid_frequency_hz"])
        grid_voltage_v = float(state["grid_voltage_v"])
        grid_on_off_status = int(state["grid_on_off_status"])
        grid_switch_position = int(state["grid_switch_position"])
        load_power_w = int(state["load_power_w"])

    grid_power_kw = (load_power_w - pv_power_w - bess_active_power_w) / 1000.0

    pv = _slave(PV_UNIT)
    pv.setValues(3, 0, _int32_words(pv_power_w))
    pv.setValues(3, 2, [int(round(pv_voltage_v * 10.0))])
    pv.setValues(3, 3, [int(round(pv_current_a * 100.0))])
    pv.setValues(3, 4, [pv_running_status])
    pv.setValues(3, 15, [1 if pv_running_status else 0])
    pv.setValues(3, 17, _int32_words(pv_target_power_w))
    pv.setValues(3, 20, [int(round(pv_target_power_limit_pct * 10.0))])

    bess = _slave(BESS_UNIT)
    bess.setValues(3, 0, [int(round(bess_soc_pct * 10.0))])
    bess.setValues(3, 1, _int32_words(bess_active_power_w))
    bess.setValues(3, 5, [bess_grid_state])
    bess.setValues(3, 6, [bess_run_mode])
    bess.setValues(1, 100, [1 if bess_started else 0])
    bess.setValues(3, 200, _int32_words(bess_target_power_w))
    bess.setValues(3, 202, [int(round(bess_target_soc_pct * 10.0))])

    grid = _slave(GRID_UNIT)
    grid.setValues(3, 0, _float32_words(grid_power_kw))
    grid.setValues(3, 2, _float32_words(grid_frequency_hz))
    grid.setValues(3, 4, _float32_words(grid_voltage_v))
    grid.setValues(3, 6, [grid_on_off_status])
    grid.setValues(3, 7, [grid_switch_position])

    with state_lock:
        state["grid_active_power_kw"] = grid_power_kw


def _reset_state() -> None:
    with state_lock:
        state.clear()
        state.update(DEFAULT_STATE)
    _sync_registers()


def _apply_state_patch(payload: dict) -> None:
    with state_lock:
        for key, value in payload.items():
            if key not in state:
                continue
            state[key] = value
    if "bess_target_power_w" in payload or "bess_target_soc_pct" in payload:
        bess = _slave(BESS_UNIT)
        with state_lock:
            bess.setValues(3, 200, _int32_words(int(state["bess_target_power_w"])))
            bess.setValues(3, 202, [int(round(float(state["bess_target_soc_pct"]) * 10.0))])
    if "bess_started" in payload:
        _slave(BESS_UNIT).setValues(1, 100, [1 if payload["bess_started"] else 0])
    _sync_registers()


def _simulation_loop(stop_event: threading.Event) -> None:
    _reset_state()
    capacity_wh = CAPACITY_KWH * 1000.0
    while not stop_event.is_set():
        _read_bess_target_state()
        with state_lock:
            target = int(state["bess_target_power_w"])
            actual = float(state["bess_active_power_w"])
            started = bool(state["bess_started"])
            run_mode = int(state["bess_run_mode"])
            soc = float(state["bess_soc_pct"])

        effective_target = target if started and run_mode != 0 else 0
        max_step = RAMP_W_PER_S * STEP_SECONDS
        if actual < effective_target:
            actual = min(actual + max_step, effective_target)
        elif actual > effective_target:
            actual = max(actual - max_step, effective_target)

        delta_soc = (-actual * STEP_SECONDS / 3600.0) / capacity_wh * 100.0
        soc = min(100.0, max(0.0, soc + delta_soc))

        with state_lock:
            state["bess_active_power_w"] = int(round(actual))
            state["bess_soc_pct"] = round(soc, 3)

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
        data = json.dumps(payload).encode("utf-8")
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
    print(f"[sim] Modbus TCP listening on 0.0.0.0:{MODBUS_PORT}", flush=True)
    print(f"[sim] HTTP control listening on 0.0.0.0:{HTTP_PORT}", flush=True)

    try:
        while not stop_event.is_set():
            httpd.handle_request()
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
