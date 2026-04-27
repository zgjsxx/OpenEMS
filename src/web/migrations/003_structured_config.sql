CREATE TABLE IF NOT EXISTS sites (
    id VARCHAR(128) PRIMARY KEY,
    name TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS ems_config (
    singleton BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK (singleton),
    log_level VARCHAR(32) NOT NULL DEFAULT 'info',
    default_poll_interval_ms INTEGER NOT NULL DEFAULT 1000,
    site_id VARCHAR(128) NOT NULL REFERENCES sites(id) ON DELETE RESTRICT,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS devices (
    id VARCHAR(128) PRIMARY KEY,
    site_id VARCHAR(128) NOT NULL REFERENCES sites(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    type VARCHAR(64) NOT NULL,
    protocol VARCHAR(64) NOT NULL,
    ip VARCHAR(128) NOT NULL DEFAULT '',
    port INTEGER NOT NULL DEFAULT 0,
    unit_id INTEGER NOT NULL DEFAULT 0,
    poll_interval_ms INTEGER NOT NULL DEFAULT 1000,
    common_address INTEGER NULL
);

CREATE INDEX IF NOT EXISTS idx_devices_site_id ON devices(site_id);
CREATE INDEX IF NOT EXISTS idx_devices_protocol ON devices(protocol);

CREATE TABLE IF NOT EXISTS points (
    id VARCHAR(128) PRIMARY KEY,
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    code TEXT NOT NULL DEFAULT '',
    category VARCHAR(32) NOT NULL,
    data_type VARCHAR(32) NOT NULL,
    unit TEXT NOT NULL DEFAULT '',
    writable BOOLEAN NOT NULL DEFAULT FALSE
);

CREATE INDEX IF NOT EXISTS idx_points_device_id ON points(device_id);
CREATE INDEX IF NOT EXISTS idx_points_category ON points(category);

CREATE TABLE IF NOT EXISTS modbus_mappings (
    point_id VARCHAR(128) PRIMARY KEY REFERENCES points(id) ON DELETE CASCADE,
    function_code INTEGER NOT NULL,
    register_address INTEGER NOT NULL,
    register_count INTEGER NOT NULL,
    data_type VARCHAR(32) NOT NULL,
    scale DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    offset_value DOUBLE PRECISION NOT NULL DEFAULT 0.0
);

CREATE TABLE IF NOT EXISTS iec104_mappings (
    point_id VARCHAR(128) PRIMARY KEY REFERENCES points(id) ON DELETE CASCADE,
    type_id INTEGER NOT NULL,
    ioa INTEGER NOT NULL,
    common_address INTEGER NOT NULL,
    scale DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    cot INTEGER NOT NULL DEFAULT 3
);

CREATE TABLE IF NOT EXISTS alarm_rules (
    id VARCHAR(128) PRIMARY KEY,
    point_id VARCHAR(128) NOT NULL REFERENCES points(id) ON DELETE CASCADE,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    operator VARCHAR(8) NOT NULL,
    threshold DOUBLE PRECISION NOT NULL,
    severity VARCHAR(32) NOT NULL,
    message TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS topology_nodes (
    id VARCHAR(128) PRIMARY KEY,
    site_id VARCHAR(128) NOT NULL REFERENCES sites(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    type VARCHAR(64) NOT NULL,
    device_id VARCHAR(128) NULL REFERENCES devices(id) ON DELETE SET NULL,
    x DOUBLE PRECISION NOT NULL,
    y DOUBLE PRECISION NOT NULL,
    enabled BOOLEAN NOT NULL DEFAULT TRUE
);

CREATE TABLE IF NOT EXISTS topology_links (
    id VARCHAR(128) PRIMARY KEY,
    site_id VARCHAR(128) NOT NULL REFERENCES sites(id) ON DELETE CASCADE,
    source_node_id VARCHAR(128) NOT NULL REFERENCES topology_nodes(id) ON DELETE CASCADE,
    target_node_id VARCHAR(128) NOT NULL REFERENCES topology_nodes(id) ON DELETE CASCADE,
    type VARCHAR(64) NOT NULL,
    enabled BOOLEAN NOT NULL DEFAULT TRUE
);

CREATE TABLE IF NOT EXISTS topology_bindings (
    id VARCHAR(128) PRIMARY KEY,
    target_type VARCHAR(16) NOT NULL,
    target_id VARCHAR(128) NOT NULL,
    point_id VARCHAR(128) NOT NULL REFERENCES points(id) ON DELETE CASCADE,
    role VARCHAR(64) NOT NULL,
    label TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_topology_bindings_point_id ON topology_bindings(point_id);

CREATE TABLE IF NOT EXISTS config_tables (
    table_name TEXT PRIMARY KEY,
    rows_json JSONB NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

DO $$
DECLARE
    rows JSONB;
BEGIN
    IF to_regclass('public.config_tables') IS NULL THEN
        RETURN;
    END IF;

    SELECT rows_json INTO rows FROM config_tables WHERE table_name = 'site';
    IF rows IS NOT NULL THEN
        INSERT INTO sites(id, name, description)
        SELECT id, name, COALESCE(description, '')
        FROM jsonb_to_recordset(rows) AS x(id TEXT, name TEXT, description TEXT)
        ON CONFLICT (id) DO UPDATE
        SET name = EXCLUDED.name,
            description = EXCLUDED.description;
    END IF;

    SELECT rows_json INTO rows FROM config_tables WHERE table_name = 'ems_config';
    IF rows IS NOT NULL THEN
        INSERT INTO ems_config(singleton, log_level, default_poll_interval_ms, site_id, updated_at)
        SELECT TRUE,
               COALESCE(log_level, 'info'),
               COALESCE(NULLIF(default_poll_interval_ms, '')::INTEGER, 1000),
               site_id,
               NOW()
        FROM jsonb_to_recordset(rows) AS x(log_level TEXT, default_poll_interval_ms TEXT, site_id TEXT)
        WHERE site_id IS NOT NULL AND site_id <> ''
        ON CONFLICT (singleton) DO UPDATE
        SET log_level = EXCLUDED.log_level,
            default_poll_interval_ms = EXCLUDED.default_poll_interval_ms,
            site_id = EXCLUDED.site_id,
            updated_at = NOW();
    END IF;

    SELECT rows_json INTO rows FROM config_tables WHERE table_name = 'device';
    IF rows IS NOT NULL THEN
        INSERT INTO devices(id, site_id, name, type, protocol, ip, port, unit_id, poll_interval_ms, common_address)
        SELECT id, site_id, name, type, protocol, COALESCE(ip, ''),
               COALESCE(NULLIF(port, '')::INTEGER, 0),
               COALESCE(NULLIF(unit_id, '')::INTEGER, 0),
               COALESCE(NULLIF(poll_interval_ms, '')::INTEGER, 1000),
               NULLIF(common_address, '')::INTEGER
        FROM jsonb_to_recordset(rows) AS x(
            id TEXT, site_id TEXT, name TEXT, type TEXT, protocol TEXT, ip TEXT,
            port TEXT, unit_id TEXT, poll_interval_ms TEXT, common_address TEXT
        )
        WHERE id IS NOT NULL AND id <> ''
        ON CONFLICT (id) DO UPDATE
        SET site_id = EXCLUDED.site_id,
            name = EXCLUDED.name,
            type = EXCLUDED.type,
            protocol = EXCLUDED.protocol,
            ip = EXCLUDED.ip,
            port = EXCLUDED.port,
            unit_id = EXCLUDED.unit_id,
            poll_interval_ms = EXCLUDED.poll_interval_ms,
            common_address = EXCLUDED.common_address;
    END IF;

END $$;

INSERT INTO points(id, device_id, name, code, category, data_type, unit, writable)
SELECT id, device_id, name, COALESCE(code, ''), 'telemetry', data_type, COALESCE(unit, ''), COALESCE(NULLIF(writable, '')::BOOLEAN, FALSE)
FROM config_tables, jsonb_to_recordset(rows_json) AS x(id TEXT, device_id TEXT, name TEXT, code TEXT, data_type TEXT, unit TEXT, writable TEXT)
WHERE table_name = 'telemetry' AND id IS NOT NULL AND id <> '' AND device_id IN (SELECT id FROM devices)
ON CONFLICT (id) DO UPDATE SET device_id=EXCLUDED.device_id, name=EXCLUDED.name, code=EXCLUDED.code, category=EXCLUDED.category, data_type=EXCLUDED.data_type, unit=EXCLUDED.unit, writable=EXCLUDED.writable;

INSERT INTO points(id, device_id, name, code, category, data_type, unit, writable)
SELECT id, device_id, name, COALESCE(code, ''), 'teleindication', data_type, COALESCE(unit, ''), COALESCE(NULLIF(writable, '')::BOOLEAN, FALSE)
FROM config_tables, jsonb_to_recordset(rows_json) AS x(id TEXT, device_id TEXT, name TEXT, code TEXT, data_type TEXT, unit TEXT, writable TEXT)
WHERE table_name = 'teleindication' AND id IS NOT NULL AND id <> '' AND device_id IN (SELECT id FROM devices)
ON CONFLICT (id) DO UPDATE SET device_id=EXCLUDED.device_id, name=EXCLUDED.name, code=EXCLUDED.code, category=EXCLUDED.category, data_type=EXCLUDED.data_type, unit=EXCLUDED.unit, writable=EXCLUDED.writable;

INSERT INTO points(id, device_id, name, code, category, data_type, unit, writable)
SELECT id, device_id, name, COALESCE(code, ''), 'telecontrol', data_type, COALESCE(unit, ''), COALESCE(NULLIF(writable, '')::BOOLEAN, FALSE)
FROM config_tables, jsonb_to_recordset(rows_json) AS x(id TEXT, device_id TEXT, name TEXT, code TEXT, data_type TEXT, unit TEXT, writable TEXT)
WHERE table_name = 'telecontrol' AND id IS NOT NULL AND id <> '' AND device_id IN (SELECT id FROM devices)
ON CONFLICT (id) DO UPDATE SET device_id=EXCLUDED.device_id, name=EXCLUDED.name, code=EXCLUDED.code, category=EXCLUDED.category, data_type=EXCLUDED.data_type, unit=EXCLUDED.unit, writable=EXCLUDED.writable;

INSERT INTO points(id, device_id, name, code, category, data_type, unit, writable)
SELECT id, device_id, name, COALESCE(code, ''), 'teleadjust', data_type, COALESCE(unit, ''), COALESCE(NULLIF(writable, '')::BOOLEAN, FALSE)
FROM config_tables, jsonb_to_recordset(rows_json) AS x(id TEXT, device_id TEXT, name TEXT, code TEXT, data_type TEXT, unit TEXT, writable TEXT)
WHERE table_name = 'teleadjust' AND id IS NOT NULL AND id <> '' AND device_id IN (SELECT id FROM devices)
ON CONFLICT (id) DO UPDATE SET device_id=EXCLUDED.device_id, name=EXCLUDED.name, code=EXCLUDED.code, category=EXCLUDED.category, data_type=EXCLUDED.data_type, unit=EXCLUDED.unit, writable=EXCLUDED.writable;

INSERT INTO modbus_mappings(point_id, function_code, register_address, register_count, data_type, scale, offset_value)
SELECT point_id, function_code::INTEGER, register_address::INTEGER, register_count::INTEGER, data_type,
       COALESCE(NULLIF(scale, '')::DOUBLE PRECISION, 1.0),
       COALESCE(NULLIF("offset", '')::DOUBLE PRECISION, 0.0)
FROM config_tables, jsonb_to_recordset(rows_json) AS x(point_id TEXT, function_code TEXT, register_address TEXT, register_count TEXT, data_type TEXT, scale TEXT, "offset" TEXT)
WHERE table_name = 'modbus_mapping' AND point_id IS NOT NULL AND point_id <> '' AND point_id IN (SELECT id FROM points)
ON CONFLICT (point_id) DO UPDATE SET function_code=EXCLUDED.function_code, register_address=EXCLUDED.register_address, register_count=EXCLUDED.register_count, data_type=EXCLUDED.data_type, scale=EXCLUDED.scale, offset_value=EXCLUDED.offset_value;

INSERT INTO iec104_mappings(point_id, type_id, ioa, common_address, scale, cot)
SELECT point_id, type_id::INTEGER, ioa::INTEGER, common_address::INTEGER,
       COALESCE(NULLIF(scale, '')::DOUBLE PRECISION, 1.0),
       COALESCE(NULLIF(cot, '')::INTEGER, 3)
FROM config_tables, jsonb_to_recordset(rows_json) AS x(point_id TEXT, type_id TEXT, ioa TEXT, common_address TEXT, scale TEXT, cot TEXT)
WHERE table_name = 'iec104_mapping' AND point_id IS NOT NULL AND point_id <> '' AND point_id IN (SELECT id FROM points)
ON CONFLICT (point_id) DO UPDATE SET type_id=EXCLUDED.type_id, ioa=EXCLUDED.ioa, common_address=EXCLUDED.common_address, scale=EXCLUDED.scale, cot=EXCLUDED.cot;

INSERT INTO alarm_rules(id, point_id, enabled, operator, threshold, severity, message)
SELECT id, point_id, COALESCE(NULLIF(enabled, '')::BOOLEAN, TRUE), operator, threshold::DOUBLE PRECISION, severity, message
FROM config_tables, jsonb_to_recordset(rows_json) AS x(id TEXT, point_id TEXT, enabled TEXT, operator TEXT, threshold TEXT, severity TEXT, message TEXT)
WHERE table_name = 'alarm_rule' AND id IS NOT NULL AND id <> '' AND point_id IN (SELECT id FROM points)
ON CONFLICT (id) DO UPDATE SET point_id=EXCLUDED.point_id, enabled=EXCLUDED.enabled, operator=EXCLUDED.operator, threshold=EXCLUDED.threshold, severity=EXCLUDED.severity, message=EXCLUDED.message;

INSERT INTO topology_nodes(id, site_id, name, type, device_id, x, y, enabled)
SELECT id, site_id, name, type,
       CASE WHEN device_id IN (SELECT devices.id FROM devices) THEN device_id ELSE NULL END,
       x::DOUBLE PRECISION, y::DOUBLE PRECISION, COALESCE(NULLIF(enabled, '')::BOOLEAN, TRUE)
FROM config_tables, jsonb_to_recordset(rows_json) AS x(id TEXT, site_id TEXT, name TEXT, type TEXT, device_id TEXT, x TEXT, y TEXT, enabled TEXT)
WHERE table_name = 'topology_node' AND id IS NOT NULL AND id <> ''
ON CONFLICT (id) DO UPDATE SET site_id=EXCLUDED.site_id, name=EXCLUDED.name, type=EXCLUDED.type, device_id=EXCLUDED.device_id, x=EXCLUDED.x, y=EXCLUDED.y, enabled=EXCLUDED.enabled;

INSERT INTO topology_links(id, site_id, source_node_id, target_node_id, type, enabled)
SELECT id, site_id, source_node_id, target_node_id, type, COALESCE(NULLIF(enabled, '')::BOOLEAN, TRUE)
FROM config_tables, jsonb_to_recordset(rows_json) AS x(id TEXT, site_id TEXT, source_node_id TEXT, target_node_id TEXT, type TEXT, enabled TEXT)
WHERE table_name = 'topology_link' AND id IS NOT NULL AND id <> ''
ON CONFLICT (id) DO UPDATE SET site_id=EXCLUDED.site_id, source_node_id=EXCLUDED.source_node_id, target_node_id=EXCLUDED.target_node_id, type=EXCLUDED.type, enabled=EXCLUDED.enabled;

INSERT INTO topology_bindings(id, target_type, target_id, point_id, role, label)
SELECT id, target_type, target_id, point_id, role, COALESCE(label, '')
FROM config_tables, jsonb_to_recordset(rows_json) AS x(id TEXT, target_type TEXT, target_id TEXT, point_id TEXT, role TEXT, label TEXT)
WHERE table_name = 'topology_binding' AND id IS NOT NULL AND id <> '' AND point_id IN (SELECT id FROM points)
ON CONFLICT (id) DO UPDATE SET target_type=EXCLUDED.target_type, target_id=EXCLUDED.target_id, point_id=EXCLUDED.point_id, role=EXCLUDED.role, label=EXCLUDED.label;

DO $$
BEGIN
    IF to_regclass('public.config_tables') IS NOT NULL THEN
        IF to_regclass('public.config_tables_legacy') IS NULL THEN
            ALTER TABLE public.config_tables RENAME TO config_tables_legacy;
        ELSE
            DROP TABLE public.config_tables;
        END IF;
    END IF;
END $$;

