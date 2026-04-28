CREATE TABLE IF NOT EXISTS strategy_definitions (
    id VARCHAR(128) PRIMARY KEY,
    name TEXT NOT NULL,
    type VARCHAR(64) NOT NULL,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    site_id VARCHAR(128) NOT NULL REFERENCES sites(id) ON DELETE CASCADE,
    device_id VARCHAR(128) NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    priority INTEGER NOT NULL DEFAULT 0,
    cycle_ms INTEGER NOT NULL DEFAULT 1000,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_strategy_definitions_site_id ON strategy_definitions(site_id);
CREATE INDEX IF NOT EXISTS idx_strategy_definitions_enabled ON strategy_definitions(enabled);

CREATE TABLE IF NOT EXISTS strategy_bindings (
    id VARCHAR(128) PRIMARY KEY,
    strategy_id VARCHAR(128) NOT NULL REFERENCES strategy_definitions(id) ON DELETE CASCADE,
    role VARCHAR(64) NOT NULL,
    point_id VARCHAR(128) NOT NULL REFERENCES points(id) ON DELETE CASCADE,
    UNIQUE(strategy_id, role)
);

CREATE INDEX IF NOT EXISTS idx_strategy_bindings_strategy_id ON strategy_bindings(strategy_id);

CREATE TABLE IF NOT EXISTS strategy_params (
    id VARCHAR(128) PRIMARY KEY,
    strategy_id VARCHAR(128) NOT NULL REFERENCES strategy_definitions(id) ON DELETE CASCADE,
    param_key VARCHAR(64) NOT NULL,
    param_value TEXT NOT NULL,
    UNIQUE(strategy_id, param_key)
);

CREATE INDEX IF NOT EXISTS idx_strategy_params_strategy_id ON strategy_params(strategy_id);

CREATE TABLE IF NOT EXISTS strategy_runtime_state (
    strategy_id VARCHAR(128) PRIMARY KEY REFERENCES strategy_definitions(id) ON DELETE CASCADE,
    last_execution_time TIMESTAMPTZ,
    current_target_value DOUBLE PRECISION,
    current_target_point_id VARCHAR(128),
    suppressed BOOLEAN NOT NULL DEFAULT FALSE,
    suppress_reason TEXT NOT NULL DEFAULT '',
    manual_override_until TIMESTAMPTZ,
    last_error TEXT NOT NULL DEFAULT '',
    input_summary JSONB NOT NULL DEFAULT '{}',
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS strategy_action_logs (
    id BIGSERIAL PRIMARY KEY,
    strategy_id VARCHAR(128) NOT NULL REFERENCES strategy_definitions(id) ON DELETE CASCADE,
    action_type VARCHAR(32) NOT NULL,
    target_point_id VARCHAR(128),
    desired_value DOUBLE PRECISION,
    result_value DOUBLE PRECISION,
    suppress_reason TEXT NOT NULL DEFAULT '',
    input_summary JSONB NOT NULL DEFAULT '{}',
    result VARCHAR(32) NOT NULL DEFAULT 'ok',
    details TEXT NOT NULL DEFAULT '',
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_strategy_action_logs_strategy_id ON strategy_action_logs(strategy_id);
CREATE INDEX IF NOT EXISTS idx_strategy_action_logs_created_at ON strategy_action_logs(created_at DESC);
