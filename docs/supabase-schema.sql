-- Ring•One Firmware · Dotstar Systems · Apache 2.0
-- Supabase device registry schema
-- Apply with: supabase db push

-- ── Users ────────────────────────────────────────────────────────────
CREATE TABLE users (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email       TEXT UNIQUE NOT NULL,
    full_name   TEXT,
    role        TEXT DEFAULT 'patient'
                CHECK (role IN ('patient', 'advisor', 'admin')),
    created_at  TIMESTAMPTZ DEFAULT NOW()
);

-- ── Device registry ──────────────────────────────────────────────────
CREATE TABLE device_registry (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    device_id       TEXT UNIQUE NOT NULL,  -- e.g. "rng-A3F2"
    user_id         UUID REFERENCES users(id) ON DELETE SET NULL,
    assigned_at     TIMESTAMPTZ DEFAULT NOW(),
    unassigned_at   TIMESTAMPTZ,
    ring_serial     TEXT,
    firmware_ver    TEXT DEFAULT '0.1.0',
    last_seen_at    TIMESTAMPTZ,
    notes           TEXT
);

-- Index: fast lookup of all active devices for a user
CREATE INDEX idx_device_registry_user
    ON device_registry(user_id)
    WHERE unassigned_at IS NULL;

-- Index: fast lookup by device_id (used by telemetry ingest validation)
CREATE INDEX idx_device_registry_device_id
    ON device_registry(device_id);

-- ── View: active assignments ─────────────────────────────────────────
CREATE VIEW active_assignments AS
    SELECT
        d.device_id,
        d.ring_serial,
        d.firmware_ver,
        d.last_seen_at,
        u.email,
        u.full_name,
        u.role
    FROM device_registry d
    JOIN users u ON d.user_id = u.id
    WHERE d.unassigned_at IS NULL;

-- ── Function: get device_ids for a user ──────────────────────────────
-- Used by the Grafana $device_id template variable to show only the
-- devices belonging to the authenticated user.
CREATE OR REPLACE FUNCTION get_user_devices(p_user_id UUID)
RETURNS TABLE(device_id TEXT) AS $$
    SELECT device_id
    FROM device_registry
    WHERE user_id = p_user_id
      AND unassigned_at IS NULL;
$$ LANGUAGE sql STABLE;

-- ── Function: update last_seen_at ────────────────────────────────────
-- Called by the telemetry ingest pipeline (e.g. Supabase Edge Function)
-- each time a telemetry packet arrives.
CREATE OR REPLACE FUNCTION update_device_last_seen(p_device_id TEXT)
RETURNS VOID AS $$
    UPDATE device_registry
    SET last_seen_at = NOW()
    WHERE device_id = p_device_id;
$$ LANGUAGE sql;

-- ── Row Level Security ───────────────────────────────────────────────
-- Patients can only see their own device; advisors/admins see all.
ALTER TABLE device_registry ENABLE ROW LEVEL SECURITY;

CREATE POLICY device_owner_policy ON device_registry
    FOR SELECT
    USING (
        user_id = auth.uid()
        OR EXISTS (
            SELECT 1 FROM users
            WHERE id = auth.uid()
              AND role IN ('advisor', 'admin')
        )
    );
