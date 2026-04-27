-- Seed initial demo data from config/tables CSV files.
-- All inserts use ON CONFLICT DO NOTHING so this is safe to re-run.

-- Sites
INSERT INTO sites(id, name, description)
VALUES ('site-demo-001', 'Demo Site', 'OpenEMS demonstration site')
ON CONFLICT (id) DO NOTHING;

-- EMS config (singleton row)
INSERT INTO ems_config(singleton, log_level, default_poll_interval_ms, site_id, updated_at)
VALUES (TRUE, 'info', 1000, 'site-demo-001', NOW())
ON CONFLICT (singleton) DO NOTHING;

-- Devices
INSERT INTO devices(id, site_id, name, type, protocol, ip, port, unit_id, poll_interval_ms, common_address)
VALUES ('pv-001', 'site-demo-001', 'PV Inverter #1', 'PV', 'modbus-tcp', '127.0.0.1', 502, 1, 1000, NULL)
ON CONFLICT (id) DO NOTHING;

INSERT INTO devices(id, site_id, name, type, protocol, ip, port, unit_id, poll_interval_ms, common_address)
VALUES ('bess-001', 'site-demo-001', 'BESS #1', 'BESS', 'modbus-tcp', '127.0.0.1', 5021, 1, 500, NULL)
ON CONFLICT (id) DO NOTHING;

INSERT INTO devices(id, site_id, name, type, protocol, ip, port, unit_id, poll_interval_ms, common_address)
VALUES ('grid-001', 'site-demo-001', 'Grid Relay #1', 'Grid', 'iec104', '127.0.0.1', 2404, 1, 1000, 1)
ON CONFLICT (id) DO NOTHING;

-- Telemetry points
INSERT INTO points(id, device_id, name, code, category, data_type, unit, writable) VALUES
('pv-active-power',  'pv-001',  'Active Power',       'PV_P',      'telemetry',      'int32',   'W',  FALSE),
('pv-voltage',       'pv-001',  'Voltage',            'PV_U',      'telemetry',      'uint16',  'V',  FALSE),
('pv-current',       'pv-001',  'Current',            'PV_I',      'telemetry',      'int16',   'A',  FALSE),
('bess-soc',         'bess-001', 'SOC',               'BESS_SOC',  'telemetry',      'uint16',  '%',  FALSE),
('bess-active-power','bess-001', 'Active Power',      'BESS_P',    'telemetry',      'int32',   'W',  FALSE),
('grid-active-power','grid-001', 'Grid Active Power',  'GRID_P',    'telemetry',      'float32', 'kW', FALSE),
('grid-frequency',   'grid-001', 'Grid Frequency',     'GRID_FREQ', 'telemetry',      'float32', 'Hz', FALSE),
('grid-voltage',     'grid-001', 'Grid Voltage',       'GRID_U',    'telemetry',      'float32', 'V',  FALSE)
ON CONFLICT (id) DO NOTHING;

-- Teleindication points
INSERT INTO points(id, device_id, name, code, category, data_type, unit, writable) VALUES
('pv-running-status',  'pv-001',  'Running Status',      'PV_RUN',     'teleindication', 'uint16', '', FALSE),
('bess-grid-state',    'bess-001', 'Grid State',          'BESS_GRID',  'teleindication', 'uint16', '', FALSE),
('bess-run-mode',      'bess-001', 'Run Mode',            'BESS_MODE',  'teleindication', 'uint16', '', TRUE),
('grid-on-off-status', 'grid-001', 'On/Off Grid Status',  'GRID_STATE', 'teleindication', 'uint16', '', FALSE),
('grid-switch-position','grid-001', 'Switch Position',     'GRID_SW',    'teleindication', 'uint16', '', FALSE)
ON CONFLICT (id) DO NOTHING;

-- Telecontrol points
INSERT INTO points(id, device_id, name, code, category, data_type, unit, writable) VALUES
('pv-start-stop',  'pv-001',  'Start/Stop', 'PV_START_STOP',  'telecontrol', 'bool', '', TRUE),
('bess-start-stop','bess-001', 'Start/Stop', 'BESS_START_STOP','telecontrol', 'bool', '', TRUE)
ON CONFLICT (id) DO NOTHING;

-- Teleadjust points
INSERT INTO points(id, device_id, name, code, category, data_type, unit, writable) VALUES
('pv-target-power',       'pv-001',  'Target Power',       'PV_TGT_P',     'teleadjust', 'int32',  'W', TRUE),
('pv-target-power-limit', 'pv-001',  'Target Power Limit', 'PV_TGT_LIMIT', 'teleadjust', 'uint16', '%', TRUE),
('bess-target-power',     'bess-001', 'Target Power',       'BESS_TGT_P',   'teleadjust', 'int32',  'W', TRUE),
('bess-target-soc',       'bess-001', 'Target SOC',         'BESS_TGT_SOC', 'teleadjust', 'uint16', '%', TRUE)
ON CONFLICT (id) DO NOTHING;

-- Modbus mappings
INSERT INTO modbus_mappings(point_id, function_code, register_address, register_count, data_type, scale, offset_value) VALUES
('pv-active-power',       3, 0,   2, 'int32',  1.0,  0.0),
('pv-voltage',            3, 2,   1, 'uint16', 0.1,  0.0),
('pv-current',            3, 3,   1, 'int16',  0.01, 0.0),
('pv-running-status',     3, 4,   1, 'uint16', 1.0,  0.0),
('pv-start-stop',         6, 15,  1, 'bool',   1.0,  0.0),
('pv-target-power',       6, 17,  2, 'int32',  1.0,  0.0),
('pv-target-power-limit', 6, 20,  1, 'uint16', 0.1,  0.0),
('bess-soc',              3, 0,   1, 'uint16', 0.1,  0.0),
('bess-active-power',     3, 1,   2, 'int32',  1.0,  0.0),
('bess-grid-state',       3, 5,   1, 'uint16', 1.0,  0.0),
('bess-run-mode',         3, 6,   1, 'uint16', 1.0,  0.0),
('bess-start-stop',       5, 100, 1, 'bool',   1.0,  0.0),
('bess-target-power',     6, 200, 2, 'int32',  1.0,  0.0),
('bess-target-soc',       6, 202, 1, 'uint16', 0.1,  0.0)
ON CONFLICT (point_id) DO NOTHING;

-- IEC104 mappings
INSERT INTO iec104_mappings(point_id, type_id, ioa, common_address, scale, cot) VALUES
('grid-on-off-status',    3,  1,   1, 1.0, 3),
('grid-active-power',     13, 100, 1, 1.0, 1),
('grid-frequency',        13, 101, 1, 1.0, 1),
('grid-voltage',          13, 102, 1, 0.1, 1),
('grid-switch-position',  1,  10,  1, 1.0, 3)
ON CONFLICT (point_id) DO NOTHING;

-- Alarm rules
INSERT INTO alarm_rules(id, point_id, enabled, operator, threshold, severity, message) VALUES
('bess-soc-low',   'bess-soc',         TRUE, '<', 10,  'critical', 'SOC too low'),
('bess-soc-high',  'bess-soc',         TRUE, '>', 95,  'warning',  'SOC too high'),
('bess-off-grid',  'bess-grid-state',   TRUE, '==', 1,  'warning',  'BESS is off-grid'),
('pv-stopped',     'pv-running-status', TRUE, '==', 0,  'warning',  'PV is stopped'),
('pv-fault',       'pv-running-status', TRUE, '==', 3,  'critical', 'PV fault')
ON CONFLICT (id) DO NOTHING;

-- Topology nodes
INSERT INTO topology_nodes(id, site_id, name, type, device_id, x, y, enabled) VALUES
('grid-incomer',   'site-demo-001', 'External Grid',   'grid',    'grid-001', 80,  180, TRUE),
('grid-breaker',   'site-demo-001', 'Grid Breaker',    'breaker', 'grid-001', 220, 180, TRUE),
('main-bus',       'site-demo-001', 'Main AC Bus',     'bus',     NULL,       380, 180, TRUE),
('pv-001-node',    'site-demo-001', 'PV Inverter #1',  'pv',      'pv-001',   560, 90,  TRUE),
('bess-001-node',  'site-demo-001', 'BESS #1',         'bess',    NULL,       560, 180, TRUE),
('load-001-node',  'site-demo-001', 'Site Load',       'load',    NULL,       560, 270, TRUE)
ON CONFLICT (id) DO NOTHING;

-- Topology links
INSERT INTO topology_links(id, site_id, source_node_id, target_node_id, type, enabled) VALUES
('grid-to-breaker', 'site-demo-001', 'grid-incomer',  'grid-breaker',   'line',    TRUE),
('breaker-to-bus',  'site-demo-001', 'grid-breaker',  'main-bus',       'breaker', TRUE),
('bus-to-pv',       'site-demo-001', 'main-bus',      'pv-001-node',    'line',    TRUE),
('bus-to-bess',     'site-demo-001', 'main-bus',      'bess-001-node',  'line',    TRUE),
('bus-to-load',     'site-demo-001', 'main-bus',      'load-001-node',  'line',    TRUE)
ON CONFLICT (id) DO NOTHING;

-- Topology bindings
INSERT INTO topology_bindings(id, target_type, target_id, point_id, role, label) VALUES
('pv-power',        'node', 'pv-001-node',    'pv-active-power',   'power',  'Active Power'),
('pv-voltage',      'node', 'pv-001-node',    'pv-voltage',        'voltage','Voltage'),
('pv-current',      'node', 'pv-001-node',    'pv-current',        'current','Current'),
('pv-status',       'node', 'pv-001-node',    'pv-running-status', 'status', 'Running Status'),
('bess-soc',        'node', 'bess-001-node',  'bess-soc',          'soc',    'SOC'),
('bess-power',      'node', 'bess-001-node',  'bess-active-power', 'power',  'Active Power'),
('bess-grid-state', 'node', 'bess-001-node',  'bess-grid-state',   'status', 'Grid State'),
('grid-power',      'node', 'grid-incomer',   'grid-active-power', 'power',  'Grid Power'),
('grid-frequency',  'node', 'grid-incomer',   'grid-frequency',    'voltage','Frequency'),
('grid-voltage',    'node', 'grid-incomer',   'grid-voltage',      'voltage','Voltage'),
('grid-switch',     'node', 'grid-breaker',   'grid-switch-position','status','Switch Position')
ON CONFLICT (id) DO NOTHING;