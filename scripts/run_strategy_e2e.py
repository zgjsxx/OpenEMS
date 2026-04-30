#!/usr/bin/env python3
"""Run the first anti-reverse-flow / SOC protection E2E validation chain."""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.parse
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
            text = raw.decode("utf-8")
            return json.loads(text)


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
    log("Logging into admin portal...")
    payload = {"username": USERNAME, "password": PASSWORD}
    result = client.request("POST", f"{BASE_URL}/api/auth/login", payload)
    if not result or not result.get("ok"):
        raise RuntimeError(f"Login failed: {result}")


def simulator_reset(client: HttpClient) -> Dict[str, Any]:
    log("Resetting simulator state...")
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
        result = predicate(snap, runtime, logs)
        return (snap, runtime, logs) if result else None

    return wait_until(label, _probe, timeout=timeout, interval=interval)


def find_runtime(rows: List[Dict[str, Any]], strategy_id: str) -> Dict[str, Any]:
    for row in rows:
        if row.get("strategy_id") == strategy_id:
            return row
    raise RuntimeError(f"Strategy runtime row not found: {strategy_id}")


def assert_anti_reverse_flow(app_client: HttpClient, sim_client: HttpClient) -> None:
    log("Running anti-reverse-flow scenario at SOC=50%...")
    simulator_patch(
        sim_client,
        {
            "bess_soc_pct": 50.0,
            "bess_active_power_w": 0,
            "bess_target_power_w": 0,
            "pv_power_w": 36000,
            "load_power_w": 10000,
            "bess_started": True,
            "bess_run_mode": 1,
            "bess_grid_state": 0,
        },
    )

    snap, runtime, logs = wait_condition(
        "anti reverse flow reaches charging state",
        app_client,
        lambda s, r, _l: read_point(s, "bess-target-power") < -5.0 and abs(read_point(s, "grid-active-power")) < 5.0,
        timeout=90,
        interval=2.0,
    )

    bess_target_kw = read_point(snap, "bess-target-power")
    grid_power_kw = read_point(snap, "grid-active-power")
    arf_runtime = find_runtime(runtime, "e2e-anti-reverse-flow")
    log(f"Anti-reverse-flow OK: bess-target-power={bess_target_kw:.2f} kW, grid-active-power={grid_power_kw:.2f} kW")
    log(f"ARF runtime: suppressed={arf_runtime.get('suppressed')} reason={arf_runtime.get('suppress_reason')}")
    if not any(row.get("strategy_id") == "e2e-anti-reverse-flow" for row in logs):
        raise RuntimeError("No anti-reverse-flow action log found.")


def assert_soc_clamp(app_client: HttpClient, sim_client: HttpClient) -> None:
    log("Running SOC high clamp scenario at SOC=90%...")
    simulator_patch(
        sim_client,
        {
            "bess_soc_pct": 90.0,
            "bess_active_power_w": 0,
            "bess_target_power_w": 0,
            "pv_power_w": 36000,
            "load_power_w": 10000,
            "bess_started": True,
            "bess_run_mode": 1,
            "bess_grid_state": 0,
        },
    )

    snap, runtime, logs = wait_condition(
        "soc high clamp keeps bess target at zero",
        app_client,
        lambda s, r, l: abs(read_point(s, "bess-target-power")) < 0.25
        and any(
            row.get("strategy_id") == "e2e-soc-protection"
            and str(row.get("suppress_reason") or "").find("charge suppressed") >= 0
            for row in r
        )
        and any(
            row.get("strategy_id") == "e2e-soc-protection"
            and str(row.get("suppress_reason") or "").find("charge suppressed") >= 0
            for row in l
        ),
        timeout=90,
        interval=2.0,
    )

    bess_target_kw = read_point(snap, "bess-target-power")
    grid_power_kw = read_point(snap, "grid-active-power")
    soc_runtime = find_runtime(runtime, "e2e-soc-protection")
    log(f"SOC clamp OK: bess-target-power={bess_target_kw:.2f} kW, grid-active-power={grid_power_kw:.2f} kW")
    log(f"SOC runtime: suppressed={soc_runtime.get('suppressed')} reason={soc_runtime.get('suppress_reason')}")


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
