#!/usr/bin/env python3
"""Run multi-device anti-reverse-flow / SOC protection E2E validation."""

from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.request
from http.cookiejar import CookieJar
from typing import Any, Dict, List


BASE_URL = os.getenv("OPENEMS_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
SIM_URL = os.getenv("OPENEMS_SIM_URL", "http://127.0.0.1:18080").rstrip("/")
USERNAME = os.getenv("OPENEMS_ADMIN_USERNAME", "admin")
PASSWORD = os.getenv("OPENEMS_ADMIN_PASSWORD", "admin123")
TIMEOUT = float(os.getenv("OPENEMS_E2E_TIMEOUT", "120"))


class HttpClient:
    def __init__(self) -> None:
        self.jar = CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def request(self, method: str, url: str, payload: Dict[str, Any] | None = None) -> Any:
        data = None
        headers = {}
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, headers=headers, method=method.upper())
        with self.opener.open(req, timeout=15) as resp:
            raw = resp.read()
            if not raw:
                return None
            return json.loads(raw.decode("utf-8"))


def log(message: str) -> None:
    print(f"[strategy-e2e] {message}", flush=True)


def wait_until(label: str, predicate, timeout: float = TIMEOUT, interval: float = 1.0):
    deadline = time.time() + timeout
    last_error = None
    while time.time() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except Exception as exc:  # noqa: BLE001
            last_error = exc
        time.sleep(interval)
    if last_error:
        raise RuntimeError(f"{label} timed out: {last_error}") from last_error
    raise RuntimeError(f"{label} timed out")


def login(client: HttpClient) -> None:
    result = client.request("POST", f"{BASE_URL}/api/auth/login", {"username": USERNAME, "password": PASSWORD})
    if not result or not result.get("ok"):
        raise RuntimeError(f"Login failed: {result}")


def simulator_reset(client: HttpClient) -> Dict[str, Any]:
    return client.request("POST", f"{SIM_URL}/reset", {}) or {}


def simulator_patch(client: HttpClient, payload: Dict[str, Any]) -> Dict[str, Any]:
    log(f"Updating simulator state: {payload}")
    return client.request("POST", f"{SIM_URL}/state", payload) or {}


def get_snapshot(client: HttpClient) -> Dict[str, Any]:
    return client.request("GET", f"{BASE_URL}/api/snapshot")


def get_strategy_runtime(client: HttpClient) -> List[Dict[str, Any]]:
    data = client.request("GET", f"{BASE_URL}/api/strategy/runtime")
    return list(data.get("rows") or [])


def get_strategy_logs(client: HttpClient) -> List[Dict[str, Any]]:
    data = client.request("GET", f"{BASE_URL}/api/strategy/logs?limit=20")
    return list(data.get("rows") or [])


def point_map(snapshot: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    return {str(point.get("id")): point for point in snapshot.get("points", [])}


def read_point(snapshot: Dict[str, Any], point_id: str) -> float:
    point = point_map(snapshot).get(point_id)
    if not point:
        raise RuntimeError(f"Point not found in snapshot: {point_id}")
    return float(point.get("value") if "value" in point else point.get("state_code") or 0.0)


def wait_point_valid(client: HttpClient, point_id: str) -> Dict[str, Any]:
    def _probe():
        snap = get_snapshot(client)
        point = point_map(snap).get(point_id)
        if point and point.get("valid"):
            return snap
        return None
    return wait_until(f"point {point_id} becomes valid", _probe, timeout=TIMEOUT, interval=1.0)


def wait_condition(label: str, client: HttpClient, predicate, timeout: float = TIMEOUT, interval: float = 1.0):
    def _probe():
        snap = get_snapshot(client)
        runtime = get_strategy_runtime(client)
        logs = get_strategy_logs(client)
        return (snap, runtime, logs) if predicate(snap, runtime, logs) else None
    return wait_until(label, _probe, timeout=timeout, interval=interval)


def find_runtime(rows: List[Dict[str, Any]], strategy_id: str) -> Dict[str, Any]:
    for row in rows:
        if row.get("strategy_id") == strategy_id:
            return row
    raise RuntimeError(f"Strategy runtime row not found: {strategy_id}")


def sum_points(snapshot: Dict[str, Any], *point_ids: str) -> float:
    return sum(read_point(snapshot, point_id) for point_id in point_ids)


def approx_equal(value: float, expected: float, tolerance: float = 0.75) -> bool:
    return abs(value - expected) <= tolerance


def assert_anti_reverse_flow(app_client: HttpClient, sim_client: HttpClient) -> None:
    log("Running multi-device anti-reverse-flow scenario...")
    simulator_patch(
        sim_client,
        {
            "bess_soc_pct": 50.0,
            "bess2_soc_pct": 55.0,
            "bess_active_power_w": 0,
            "bess2_active_power_w": 0,
            "bess_target_power_w": 0,
            "bess2_target_power_w": 0,
            "pv_available_power_w": 18000,
            "pv2_available_power_w": 18000,
            "load_power_w": 10000,
            "bess_started": True,
            "bess2_started": True,
            "bess_run_mode": 1,
            "bess2_run_mode": 1,
            "bess_grid_state": 0,
            "bess2_grid_state": 0,
        },
    )

    snap, runtime, logs = wait_condition(
        "anti reverse flow reaches charging state",
        app_client,
        lambda s, r, _l: sum_points(s, "bess-target-power", "bess2-target-power") < -5.0
        and abs(read_point(s, "grid-active-power")) < 5.0,
        timeout=90,
        interval=2.0,
    )

    total_bess_target_kw = sum_points(snap, "bess-target-power", "bess2-target-power")
    bess1_target_kw = read_point(snap, "bess-target-power")
    bess2_target_kw = read_point(snap, "bess2-target-power")
    grid_power_kw = read_point(snap, "grid-active-power")
    arf_runtime = find_runtime(runtime, "e2e-anti-reverse-flow")
    if not approx_equal(bess1_target_kw, -9.75) or not approx_equal(bess2_target_kw, -16.25):
        raise RuntimeError(
            "BESS max-power distribution mismatch: "
            f"bess1={bess1_target_kw:.2f} kW bess2={bess2_target_kw:.2f} kW"
        )
    log(
        f"Anti-reverse-flow OK: total-bess-target={total_bess_target_kw:.2f} kW, "
        f"bess1={bess1_target_kw:.2f} kW, bess2={bess2_target_kw:.2f} kW, "
        f"grid-active-power={grid_power_kw:.2f} kW"
    )
    log(f"ARF runtime: suppressed={arf_runtime.get('suppressed')} reason={arf_runtime.get('suppress_reason')}")
    if not any(row.get("strategy_id") == "e2e-anti-reverse-flow" for row in logs):
        raise RuntimeError("No anti-reverse-flow action log found.")


def assert_soc_clamp(app_client: HttpClient, sim_client: HttpClient) -> None:
    log("Running multi-device SOC high clamp with PV curtailment scenario...")
    simulator_patch(
        sim_client,
        {
            "bess_soc_pct": 90.0,
            "bess2_soc_pct": 92.0,
            "bess_active_power_w": 0,
            "bess2_active_power_w": 0,
            "bess_target_power_w": 0,
            "bess2_target_power_w": 0,
            "pv_available_power_w": 18000,
            "pv2_available_power_w": 18000,
            "load_power_w": 10000,
            "bess_started": True,
            "bess2_started": True,
            "bess_run_mode": 1,
            "bess2_run_mode": 1,
            "bess_grid_state": 0,
            "bess2_grid_state": 0,
        },
    )

    snap, runtime, logs = wait_condition(
        "soc high clamp hands over to pv curtailment",
        app_client,
        lambda s, r, l: abs(sum_points(s, "bess-target-power", "bess2-target-power")) < 0.5
        and abs(sum_points(s, "bess-active-power", "bess2-active-power")) < 0.5
        and read_point(s, "pv-target-power-limit") < 99.5
        and read_point(s, "pv2-target-power-limit") < 99.5
        and approx_equal(read_point(s, "pv-active-power") / 1000.0, 4.0)
        and approx_equal(read_point(s, "pv2-active-power") / 1000.0, 6.0)
        and abs(read_point(s, "grid-active-power")) < 5.0
        and any(
            row.get("strategy_id") == "e2e-anti-reverse-flow"
            and "pv-target-power-limit" in str(row.get("target_point_id") or "")
            for row in l
        ),
        timeout=90,
        interval=2.0,
    )

    total_bess_target_kw = sum_points(snap, "bess-target-power", "bess2-target-power")
    total_bess_actual_kw = sum_points(snap, "bess-active-power", "bess2-active-power")
    grid_power_kw = read_point(snap, "grid-active-power")
    pv1_limit_pct = read_point(snap, "pv-target-power-limit")
    pv2_limit_pct = read_point(snap, "pv2-target-power-limit")
    pv1_actual_kw = read_point(snap, "pv-active-power") / 1000.0
    pv2_actual_kw = read_point(snap, "pv2-active-power") / 1000.0
    soc_runtime = find_runtime(runtime, "e2e-soc-protection")
    arf_runtime = find_runtime(runtime, "e2e-anti-reverse-flow")
    if not approx_equal(pv1_actual_kw, 4.0) or not approx_equal(pv2_actual_kw, 6.0):
        raise RuntimeError(
            "PV rated-power curtailment mismatch: "
            f"pv1={pv1_actual_kw:.2f} kW pv2={pv2_actual_kw:.2f} kW"
        )
    log(
        "SOC clamp + PV curtailment OK: "
        f"total-bess-target={total_bess_target_kw:.2f} kW, "
        f"total-bess-active={total_bess_actual_kw:.2f} kW, "
        f"pv1-limit={pv1_limit_pct:.2f} %, pv2-limit={pv2_limit_pct:.2f} %, "
        f"pv1={pv1_actual_kw:.2f} kW, pv2={pv2_actual_kw:.2f} kW, "
        f"grid-active-power={grid_power_kw:.2f} kW"
    )
    log(f"SOC runtime: suppressed={soc_runtime.get('suppressed')} reason={soc_runtime.get('suppress_reason')}")
    log(f"ARF runtime: target_point={arf_runtime.get('current_target_point_id')} value={arf_runtime.get('current_target_value')}")


def main() -> int:
    app = HttpClient()
    sim = HttpClient()
    try:
        wait_until(
            "simulator HTTP becomes ready",
            lambda: sim.request("GET", f"{SIM_URL}/state"),
            timeout=TIMEOUT,
            interval=2.0,
        )
        wait_until(
            "admin portal becomes ready",
            lambda: urllib.request.urlopen(f"{BASE_URL}/login", timeout=10).status == 200,
            timeout=TIMEOUT,
            interval=2.0,
        )
        login(app)
        simulator_reset(sim)
        wait_point_valid(app, "grid-active-power")
        assert_anti_reverse_flow(app, sim)
        assert_soc_clamp(app, sim)
        log("E2E validation passed.")
        return 0
    except (RuntimeError, urllib.error.URLError, urllib.error.HTTPError, json.JSONDecodeError) as exc:
        log(f"E2E validation failed: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
