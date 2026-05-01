DELETE FROM strategy_action_logs;
DELETE FROM strategy_runtime_state;
DELETE FROM strategy_params;
DELETE FROM strategy_bindings;
DELETE FROM strategy_definitions;

INSERT INTO strategy_definitions(id, name, type, enabled, site_id, device_id, priority, cycle_ms)
VALUES
    ('e2e-anti-reverse-flow', 'E2E Anti Reverse Flow (Multi Device)', 'anti_reverse_flow', TRUE, 'site-demo-001', 'bess-001', 10, 1000),
    ('e2e-soc-protection', 'E2E SOC Protection (Multi Device)', 'soc_protection', TRUE, 'site-demo-001', 'bess-001', 20, 1000);

INSERT INTO strategy_bindings(id, strategy_id, role, point_id)
VALUES
    ('e2e-arf-grid-power', 'e2e-anti-reverse-flow', 'grid_power', 'grid-active-power'),
    ('e2e-arf-bess1-power', 'e2e-anti-reverse-flow', 'bess_power#bess-001', 'bess-active-power'),
    ('e2e-arf-bess1-run', 'e2e-anti-reverse-flow', 'bess_run_state#bess-001', 'bess-run-mode'),
    ('e2e-arf-bess1-setpoint', 'e2e-anti-reverse-flow', 'bess_power_setpoint#bess-001', 'bess-target-power'),
    ('e2e-arf-bess2-power', 'e2e-anti-reverse-flow', 'bess_power#bess-002', 'bess2-active-power'),
    ('e2e-arf-bess2-run', 'e2e-anti-reverse-flow', 'bess_run_state#bess-002', 'bess2-run-mode'),
    ('e2e-arf-bess2-setpoint', 'e2e-anti-reverse-flow', 'bess_power_setpoint#bess-002', 'bess2-target-power'),
    ('e2e-arf-pv1-power', 'e2e-anti-reverse-flow', 'pv_power#pv-001', 'pv-active-power'),
    ('e2e-arf-pv1-limit', 'e2e-anti-reverse-flow', 'pv_power_limit_setpoint#pv-001', 'pv-target-power-limit'),
    ('e2e-arf-pv1-run', 'e2e-anti-reverse-flow', 'pv_run_state#pv-001', 'pv-running-status'),
    ('e2e-arf-pv2-power', 'e2e-anti-reverse-flow', 'pv_power#pv-002', 'pv2-active-power'),
    ('e2e-arf-pv2-limit', 'e2e-anti-reverse-flow', 'pv_power_limit_setpoint#pv-002', 'pv2-target-power-limit'),
    ('e2e-arf-pv2-run', 'e2e-anti-reverse-flow', 'pv_run_state#pv-002', 'pv2-running-status'),
    ('e2e-soc-bess1-soc', 'e2e-soc-protection', 'bess_soc#bess-001', 'bess-soc'),
    ('e2e-soc-bess1-power', 'e2e-soc-protection', 'bess_power#bess-001', 'bess-active-power'),
    ('e2e-soc-bess1-run', 'e2e-soc-protection', 'bess_run_state#bess-001', 'bess-run-mode'),
    ('e2e-soc-bess1-setpoint', 'e2e-soc-protection', 'bess_power_setpoint#bess-001', 'bess-target-power'),
    ('e2e-soc-bess2-soc', 'e2e-soc-protection', 'bess_soc#bess-002', 'bess2-soc'),
    ('e2e-soc-bess2-power', 'e2e-soc-protection', 'bess_power#bess-002', 'bess2-active-power'),
    ('e2e-soc-bess2-run', 'e2e-soc-protection', 'bess_run_state#bess-002', 'bess2-run-mode'),
    ('e2e-soc-bess2-setpoint', 'e2e-soc-protection', 'bess_power_setpoint#bess-002', 'bess2-target-power');

INSERT INTO strategy_params(id, strategy_id, param_key, param_value)
VALUES
    ('e2e-arf-export-limit', 'e2e-anti-reverse-flow', 'export_limit_kw', '0'),
    ('e2e-arf-max-charge', 'e2e-anti-reverse-flow', 'max_charge_kw', '160'),
    ('e2e-arf-max-discharge', 'e2e-anti-reverse-flow', 'max_discharge_kw', '160'),
    ('e2e-arf-deadband', 'e2e-anti-reverse-flow', 'deadband_kw', '0.2'),
    ('e2e-arf-ramp-rate', 'e2e-anti-reverse-flow', 'ramp_rate_kw_per_s', '80'),
    ('e2e-arf-pv-enable', 'e2e-anti-reverse-flow', 'enable_pv_curtailment', 'true'),
    ('e2e-arf-pv-rated', 'e2e-anti-reverse-flow', 'pv_rated_power_kw', '200'),
    ('e2e-arf-pv1-rated', 'e2e-anti-reverse-flow', 'pv_rated_power_kw#pv-001', '80'),
    ('e2e-arf-pv2-rated', 'e2e-anti-reverse-flow', 'pv_rated_power_kw#pv-002', '120'),
    ('e2e-arf-bess1-max-charge', 'e2e-anti-reverse-flow', 'bess_max_charge_kw#bess-001', '60'),
    ('e2e-arf-bess2-max-charge', 'e2e-anti-reverse-flow', 'bess_max_charge_kw#bess-002', '100'),
    ('e2e-arf-bess1-max-discharge', 'e2e-anti-reverse-flow', 'bess_max_discharge_kw#bess-001', '60'),
    ('e2e-arf-bess2-max-discharge', 'e2e-anti-reverse-flow', 'bess_max_discharge_kw#bess-002', '100'),
    ('e2e-arf-pv-min', 'e2e-anti-reverse-flow', 'pv_limit_min_pct', '0'),
    ('e2e-arf-pv-max', 'e2e-anti-reverse-flow', 'pv_limit_max_pct', '100'),
    ('e2e-arf-pv-recovery', 'e2e-anti-reverse-flow', 'pv_limit_recovery_step_pct', '10'),
    ('e2e-arf-manual', 'e2e-anti-reverse-flow', 'manual_override_minutes', '30'),
    ('e2e-soc-low', 'e2e-soc-protection', 'soc_low', '20'),
    ('e2e-soc-high', 'e2e-soc-protection', 'soc_high', '80'),
    ('e2e-soc-deadband', 'e2e-soc-protection', 'deadband_kw', '0.2'),
    ('e2e-soc-manual', 'e2e-soc-protection', 'manual_override_minutes', '30');
