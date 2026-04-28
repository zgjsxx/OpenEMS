-- 005_history_timescale.sql
-- Create TimescaleDB hypertable for history samples and ingestion progress tracking.

CREATE EXTENSION IF NOT EXISTS timescaledb;

CREATE TABLE IF NOT EXISTS history_samples (
    ts          TIMESTAMPTZ NOT NULL,
    site_id     VARCHAR(128) NOT NULL DEFAULT '',
    point_id    VARCHAR(128) NOT NULL,
    device_id   VARCHAR(128) NOT NULL DEFAULT '',
    category    VARCHAR(32)  NOT NULL DEFAULT '',
    value       DOUBLE PRECISION NOT NULL DEFAULT 0,
    unit        VARCHAR(64)  NOT NULL DEFAULT '',
    quality     VARCHAR(32)  NOT NULL DEFAULT 'Unknown',
    valid       BOOLEAN      NOT NULL DEFAULT FALSE
);

SELECT create_hypertable('history_samples', 'ts',
    chunk_time_interval => INTERVAL '1 day',
    if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS idx_history_samples_point_id_ts
    ON history_samples (point_id, ts DESC);
CREATE INDEX IF NOT EXISTS idx_history_samples_device_id_ts
    ON history_samples (device_id, ts DESC);

-- Compression: auto-compress chunks older than 7 days
ALTER TABLE history_samples SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'point_id, site_id',
    timescaledb.compress_orderby = 'ts DESC'
);

SELECT add_compression_policy('history_samples', INTERVAL '7 days',
    if_not_exists => TRUE);

-- Retention: drop chunks older than 365 days (optional, can be adjusted)
SELECT add_retention_policy('history_samples', INTERVAL '365 days',
    if_not_exists => TRUE);

-- Track which JSONL files have been ingested into TimescaleDB
CREATE TABLE IF NOT EXISTS history_ingestion_progress (
    filename    VARCHAR(256) PRIMARY KEY,
    file_size   BIGINT      NOT NULL DEFAULT 0,
    line_count  INTEGER     NOT NULL DEFAULT 0,
    byte_offset BIGINT      NOT NULL DEFAULT 0,
    ingested_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);