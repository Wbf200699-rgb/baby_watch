from __future__ import annotations

import json
import sqlite3
import threading
import time
import uuid
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


class Database:
    COMMAND_ENQUEUE_RETRY_SECONDS = 12.0
    COMMAND_ENQUEUE_ATTEMPT_SECONDS = 1.0
    WRITE_RETRY_SECONDS = 3.0
    WRITE_ATTEMPT_SECONDS = 0.25

    def __init__(self, path: Path):
        self.path = path
        self._init_lock = threading.Lock()

    @contextmanager
    def connect(self, *, timeout_seconds: float = 10.0) -> Iterator[sqlite3.Connection]:
        timeout_seconds = max(0.05, float(timeout_seconds))
        connection = sqlite3.connect(self.path, timeout=timeout_seconds)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute(f"PRAGMA busy_timeout = {int(timeout_seconds * 1000)}")
        try:
            yield connection
            connection.commit()
        finally:
            connection.close()

    def initialize(self) -> None:
        with self._init_lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with self.connect() as db:
                # Journal mode is persistent. Reapplying it on every connection can
                # contend with the telemetry writer and delay interactive commands.
                db.execute("PRAGMA journal_mode = WAL")
                db.executescript(
                    """
                    CREATE TABLE IF NOT EXISTS devices (
                        device_id TEXT PRIMARY KEY,
                        name TEXT NOT NULL,
                        first_seen TEXT NOT NULL,
                        last_seen TEXT NOT NULL,
                        firmware TEXT NOT NULL DEFAULT '',
                        hardware TEXT NOT NULL DEFAULT '',
                        ip_address TEXT NOT NULL DEFAULT '',
                        wifi_rssi INTEGER,
                        capabilities_json TEXT NOT NULL DEFAULT '[]',
                        metadata_json TEXT NOT NULL DEFAULT '{}'
                    );

                    CREATE TABLE IF NOT EXISTS telemetry (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        device_id TEXT NOT NULL REFERENCES devices(device_id),
                        sequence INTEGER NOT NULL,
                        received_at TEXT NOT NULL,
                        device_time_ms INTEGER NOT NULL,
                        ra8_tick INTEGER NOT NULL,
                        payload_json TEXT NOT NULL
                    );
                    CREATE INDEX IF NOT EXISTS idx_telemetry_device_time
                        ON telemetry(device_id, id DESC);

                    CREATE TABLE IF NOT EXISTS events (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        device_id TEXT NOT NULL REFERENCES devices(device_id),
                        source_event_id INTEGER NOT NULL,
                        event_type TEXT NOT NULL,
                        state TEXT NOT NULL,
                        severity INTEGER NOT NULL,
                        confidence REAL NOT NULL,
                        occurred_at_ms INTEGER NOT NULL,
                        received_at TEXT NOT NULL,
                        payload_json TEXT NOT NULL,
                        acknowledged INTEGER NOT NULL DEFAULT 0,
                        UNIQUE(device_id, source_event_id)
                    );

                    CREATE TABLE IF NOT EXISTS device_logs (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        device_id TEXT NOT NULL REFERENCES devices(device_id),
                        received_at TEXT NOT NULL,
                        device_time_ms INTEGER NOT NULL,
                        level TEXT NOT NULL,
                        component TEXT NOT NULL,
                        message TEXT NOT NULL,
                        context_json TEXT NOT NULL
                    );
                    CREATE INDEX IF NOT EXISTS idx_device_logs_device_time
                        ON device_logs(device_id, id DESC);

                    CREATE TABLE IF NOT EXISTS matrices (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        device_id TEXT NOT NULL REFERENCES devices(device_id),
                        kind TEXT NOT NULL,
                        sequence INTEGER NOT NULL,
                        rows INTEGER NOT NULL,
                        cols INTEGER NOT NULL,
                        unit TEXT NOT NULL,
                        received_at TEXT NOT NULL,
                        minimum INTEGER NOT NULL,
                        maximum INTEGER NOT NULL,
                        data BLOB NOT NULL
                    );
                    CREATE INDEX IF NOT EXISTS idx_matrix_latest
                        ON matrices(device_id, kind, id DESC);

                    CREATE TABLE IF NOT EXISTS collection_sessions (
                        session_id TEXT PRIMARY KEY,
                        name TEXT NOT NULL,
                        description TEXT NOT NULL,
                        status TEXT NOT NULL,
                        created_at TEXT NOT NULL,
                        started_at TEXT,
                        ended_at TEXT,
                        target_labels_json TEXT NOT NULL,
                        metadata_json TEXT NOT NULL
                    );

                    CREATE TABLE IF NOT EXISTS samples (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        session_id TEXT NOT NULL REFERENCES collection_sessions(session_id),
                        device_id TEXT NOT NULL,
                        captured_at TEXT NOT NULL,
                        kind TEXT NOT NULL,
                        source_ref TEXT NOT NULL,
                        features_json TEXT NOT NULL,
                        label TEXT,
                        notes TEXT NOT NULL DEFAULT '',
                        quality TEXT NOT NULL DEFAULT 'unreviewed',
                        split TEXT NOT NULL DEFAULT 'unassigned'
                    );
                    CREATE INDEX IF NOT EXISTS idx_samples_session
                        ON samples(session_id, id DESC);
                    CREATE INDEX IF NOT EXISTS idx_samples_session_source
                        ON samples(session_id, source_ref);

                    CREATE TABLE IF NOT EXISTS artifacts (
                        artifact_id TEXT PRIMARY KEY,
                        session_id TEXT,
                        device_id TEXT NOT NULL,
                        kind TEXT NOT NULL,
                        mime_type TEXT NOT NULL,
                        relative_path TEXT NOT NULL,
                        sha256 TEXT NOT NULL,
                        size_bytes INTEGER NOT NULL,
                        captured_at TEXT NOT NULL,
                        metadata_json TEXT NOT NULL
                    );

                    CREATE TABLE IF NOT EXISTS commands (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        device_id TEXT NOT NULL,
                        command_json TEXT NOT NULL,
                        client_request_id TEXT,
                        status TEXT NOT NULL,
                        created_at TEXT NOT NULL,
                        sent_at TEXT,
                        acked_at TEXT,
                        result_json TEXT,
                        delivery_attempts INTEGER NOT NULL DEFAULT 0
                    );

                    CREATE TABLE IF NOT EXISTS model_packages (
                        model_id TEXT PRIMARY KEY,
                        name TEXT NOT NULL,
                        version TEXT NOT NULL,
                        target TEXT NOT NULL,
                        task TEXT NOT NULL,
                        relative_path TEXT NOT NULL,
                        sha256 TEXT NOT NULL,
                        size_bytes INTEGER NOT NULL,
                        created_at TEXT NOT NULL,
                        metrics_json TEXT NOT NULL,
                        metadata_json TEXT NOT NULL
                    );

                    CREATE TABLE IF NOT EXISTS overlay_calibrations (
                        device_id TEXT PRIMARY KEY,
                        enabled INTEGER NOT NULL DEFAULT 1,
                        mode TEXT NOT NULL DEFAULT 'identity',
                        matrix_json TEXT NOT NULL,
                        source_points_json TEXT NOT NULL,
                        target_points_json TEXT NOT NULL,
                        thermal_enabled INTEGER NOT NULL DEFAULT 0,
                        thermal_matrix_json TEXT NOT NULL DEFAULT '[1,0,0,0,1,0,0,0,1]',
                        thermal_source_points_json TEXT NOT NULL DEFAULT '[[0.25,0.25],[0.75,0.75]]',
                        thermal_target_points_json TEXT NOT NULL DEFAULT '[[0.25,0.25],[0.75,0.75]]',
                        thermal_opacity REAL NOT NULL DEFAULT 0.32,
                        updated_at TEXT NOT NULL
                    );

                    CREATE TABLE IF NOT EXISTS environment_thresholds (
                        device_id TEXT PRIMARY KEY,
                        co2_max_ppm INTEGER NOT NULL,
                        temperature_max_milli_c INTEGER NOT NULL,
                        humidity_max_milli_percent INTEGER NOT NULL,
                        humidity_min_milli_percent INTEGER NOT NULL DEFAULT 30000,
                        gas_min_ohm INTEGER NOT NULL,
                        body_temperature_max_deci_c INTEGER NOT NULL DEFAULT 375,
                        updated_at TEXT NOT NULL
                    );

                    CREATE TABLE IF NOT EXISTS ai_story_jobs (
                        story_id TEXT PRIMARY KEY,
                        device_id TEXT NOT NULL,
                        request_id TEXT,
                        status TEXT NOT NULL,
                        request_json TEXT NOT NULL,
                        title TEXT NOT NULL DEFAULT '',
                        story_text TEXT NOT NULL DEFAULT '',
                        model TEXT NOT NULL DEFAULT '',
                        tts_model TEXT NOT NULL DEFAULT '',
                        audio_relative_path TEXT NOT NULL DEFAULT '',
                        audio_sha256 TEXT NOT NULL DEFAULT '',
                        audio_crc32 INTEGER NOT NULL DEFAULT 0,
                        audio_size_bytes INTEGER NOT NULL DEFAULT 0,
                        audio_duration_ms INTEGER NOT NULL DEFAULT 0,
                        command_id INTEGER,
                        error TEXT NOT NULL DEFAULT '',
                        created_at TEXT NOT NULL,
                        updated_at TEXT NOT NULL
                    );
                    CREATE UNIQUE INDEX IF NOT EXISTS idx_ai_story_request
                        ON ai_story_jobs(device_id, request_id) WHERE request_id IS NOT NULL;
                    CREATE INDEX IF NOT EXISTS idx_ai_story_device_time
                        ON ai_story_jobs(device_id, created_at DESC);

                    CREATE TABLE IF NOT EXISTS ai_advisories (
                        advisory_id TEXT PRIMARY KEY,
                        device_id TEXT NOT NULL,
                        level TEXT NOT NULL,
                        title TEXT NOT NULL,
                        summary TEXT NOT NULL,
                        evidence_json TEXT NOT NULL,
                        actions_json TEXT NOT NULL,
                        confidence REAL NOT NULL,
                        requires_user_attention INTEGER NOT NULL DEFAULT 0,
                        source_sequence INTEGER NOT NULL DEFAULT 0,
                        source_fingerprint TEXT NOT NULL,
                        model TEXT NOT NULL,
                        acknowledged INTEGER NOT NULL DEFAULT 0,
                        created_at TEXT NOT NULL
                    );
                    CREATE INDEX IF NOT EXISTS idx_ai_advisory_device_time
                        ON ai_advisories(device_id, created_at DESC);
                    """
                )
                command_columns = {row["name"] for row in db.execute("PRAGMA table_info(commands)")}
                if "delivery_attempts" not in command_columns:
                    db.execute("ALTER TABLE commands ADD COLUMN delivery_attempts INTEGER NOT NULL DEFAULT 0")
                if "client_request_id" not in command_columns:
                    db.execute("ALTER TABLE commands ADD COLUMN client_request_id TEXT")
                db.execute(
                    "CREATE UNIQUE INDEX IF NOT EXISTS idx_commands_client_request "
                    "ON commands(device_id, client_request_id) WHERE client_request_id IS NOT NULL"
                )
                sample_columns = {row["name"] for row in db.execute("PRAGMA table_info(samples)")}
                if "split" not in sample_columns:
                    db.execute("ALTER TABLE samples ADD COLUMN split TEXT NOT NULL DEFAULT 'unassigned'")
                overlay_columns = {
                    row["name"] for row in db.execute("PRAGMA table_info(overlay_calibrations)")
                }
                overlay_migrations = {
                    "thermal_enabled": "INTEGER NOT NULL DEFAULT 0",
                    "thermal_matrix_json": "TEXT NOT NULL DEFAULT '[1,0,0,0,1,0,0,0,1]'",
                    "thermal_source_points_json": (
                        "TEXT NOT NULL DEFAULT '[[0.25,0.25],[0.75,0.75]]'"
                    ),
                    "thermal_target_points_json": (
                        "TEXT NOT NULL DEFAULT '[[0.25,0.25],[0.75,0.75]]'"
                    ),
                    "thermal_opacity": "REAL NOT NULL DEFAULT 0.32",
                }
                for column, declaration in overlay_migrations.items():
                    if column not in overlay_columns:
                        db.execute(
                            f"ALTER TABLE overlay_calibrations ADD COLUMN {column} {declaration}"
                        )
                environment_columns = {
                    row["name"] for row in db.execute("PRAGMA table_info(environment_thresholds)")
                }
                if "body_temperature_max_deci_c" not in environment_columns:
                    db.execute(
                        "ALTER TABLE environment_thresholds ADD COLUMN "
                        "body_temperature_max_deci_c INTEGER NOT NULL DEFAULT 375"
                    )
                if "humidity_min_milli_percent" not in environment_columns:
                    db.execute(
                        "ALTER TABLE environment_thresholds ADD COLUMN "
                        "humidity_min_milli_percent INTEGER NOT NULL DEFAULT 30000"
                    )

    def execute(self, sql: str, parameters: tuple[Any, ...] = ()) -> int:
        deadline = time.monotonic() + self.WRITE_RETRY_SECONDS
        delay_seconds = 0.02

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise sqlite3.OperationalError("database remained locked while writing")
            try:
                with self.connect(
                    timeout_seconds=min(self.WRITE_ATTEMPT_SECONDS, remaining)
                ) as db:
                    cursor = db.execute(sql, parameters)
                    return int(cursor.lastrowid or 0)
            except sqlite3.OperationalError as error:
                message = str(error).lower()
                if "locked" not in message and "busy" not in message:
                    raise
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise
                time.sleep(min(delay_seconds, remaining))
                delay_seconds = min(delay_seconds * 2.0, 0.25)

    def query_all(self, sql: str, parameters: tuple[Any, ...] = ()) -> list[dict[str, Any]]:
        with self.connect() as db:
            return [dict(row) for row in db.execute(sql, parameters).fetchall()]

    def query_one(self, sql: str, parameters: tuple[Any, ...] = ()) -> dict[str, Any] | None:
        rows = self.query_all(sql, parameters)
        return rows[0] if rows else None

    def touch_device(
        self,
        device_id: str,
        *,
        firmware: str = "",
        hardware: str = "",
        ip_address: str = "",
        wifi_rssi: int | None = None,
        capabilities: list[str] | None = None,
        metadata: dict[str, Any] | None = None,
    ) -> None:
        now = utc_now()
        with self.connect() as db:
            db.execute(
                """
                INSERT INTO devices(device_id, name, first_seen, last_seen, firmware, hardware,
                                    ip_address, wifi_rssi, capabilities_json, metadata_json)
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    last_seen=excluded.last_seen,
                    firmware=CASE WHEN excluded.firmware='' THEN devices.firmware ELSE excluded.firmware END,
                    hardware=CASE WHEN excluded.hardware='' THEN devices.hardware ELSE excluded.hardware END,
                    ip_address=excluded.ip_address,
                    wifi_rssi=COALESCE(excluded.wifi_rssi, devices.wifi_rssi),
                    capabilities_json=CASE WHEN excluded.capabilities_json='[]' THEN devices.capabilities_json ELSE excluded.capabilities_json END,
                    metadata_json=CASE WHEN excluded.metadata_json='{}' THEN devices.metadata_json ELSE excluded.metadata_json END
                """,
                (
                    device_id,
                    device_id,
                    now,
                    now,
                    firmware,
                    hardware,
                    ip_address,
                    wifi_rssi,
                    json.dumps(capabilities or [], separators=(",", ":")),
                    json.dumps(metadata or {}, separators=(",", ":")),
                ),
            )

    def active_sessions(self) -> list[dict[str, Any]]:
        return self.query_all("SELECT * FROM collection_sessions WHERE status='active' ORDER BY started_at")

    def claim_command(self, device_id: str, redeliver_before: str, sent_at: str) -> dict[str, Any] | None:
        with self.connect() as db:
            row = db.execute(
                """
                SELECT * FROM commands
                WHERE device_id=? AND
                      (status='queued' OR (status='sent' AND sent_at IS NOT NULL AND sent_at<=?))
                ORDER BY id LIMIT 1
                """,
                (device_id, redeliver_before),
            ).fetchone()
            if row is None:
                return None

            delivery_attempts = int(row["delivery_attempts"] or 0) + 1
            updated = db.execute(
                """
                UPDATE commands SET status='sent', sent_at=?, delivery_attempts=?
                WHERE id=? AND device_id=? AND
                      (status='queued' OR (status='sent' AND sent_at IS NOT NULL AND sent_at<=?))
                """,
                (sent_at, delivery_attempts, row["id"], device_id, redeliver_before),
            )
            if updated.rowcount != 1:
                return None
            command = dict(row)
            command["status"] = "sent"
            command["sent_at"] = sent_at
            command["delivery_attempts"] = delivery_attempts
            return command

    def enqueue_command(
        self,
        device_id: str,
        command_json: str,
        created_at: str,
        client_request_id: str | None = None,
    ) -> tuple[dict[str, Any], bool]:
        deadline = time.monotonic() + self.COMMAND_ENQUEUE_RETRY_SECONDS
        delay_seconds = 0.05

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise sqlite3.OperationalError("database remained locked while enqueueing command")
            try:
                return self._enqueue_command_once(
                    device_id,
                    command_json,
                    created_at,
                    client_request_id,
                    timeout_seconds=min(self.COMMAND_ENQUEUE_ATTEMPT_SECONDS, remaining),
                )
            except sqlite3.OperationalError as error:
                message = str(error).lower()
                if "locked" not in message and "busy" not in message:
                    raise
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise
                time.sleep(min(delay_seconds, remaining))
                delay_seconds = min(delay_seconds * 2.0, 0.5)

    def _enqueue_command_once(
        self,
        device_id: str,
        command_json: str,
        created_at: str,
        client_request_id: str | None,
        *,
        timeout_seconds: float,
    ) -> tuple[dict[str, Any], bool]:
        with self.connect(timeout_seconds=timeout_seconds) as db:
            db.execute("BEGIN IMMEDIATE")
            if client_request_id:
                existing = db.execute(
                    "SELECT * FROM commands WHERE device_id=? AND client_request_id=?",
                    (device_id, client_request_id),
                ).fetchone()
                if existing is not None:
                    return dict(existing), True
            cursor = db.execute(
                "INSERT INTO commands(device_id, command_json, client_request_id, status, created_at) "
                "VALUES(?, ?, ?, 'queued', ?)",
                (device_id, command_json, client_request_id, created_at),
            )
            row = db.execute("SELECT * FROM commands WHERE id=?", (cursor.lastrowid,)).fetchone()
            if row is None:
                raise RuntimeError("command insert did not return a row")
            return dict(row), False

    def prune_runtime_data(
        self,
        device_id: str,
        *,
        telemetry_rows: int,
        log_rows: int,
        matrix_rows: int,
    ) -> None:
        telemetry_rows = max(1, telemetry_rows)
        log_rows = max(1, log_rows)
        matrix_rows = max(1, matrix_rows)
        with self.connect() as db:
            db.execute(
                """
                DELETE FROM telemetry
                WHERE device_id=? AND id NOT IN (
                    SELECT id FROM telemetry WHERE device_id=? ORDER BY id DESC LIMIT ?
                )
                """,
                (device_id, device_id, telemetry_rows),
            )
            db.execute(
                """
                DELETE FROM device_logs
                WHERE device_id=? AND id NOT IN (
                    SELECT id FROM device_logs WHERE device_id=? ORDER BY id DESC LIMIT ?
                )
                """,
                (device_id, device_id, log_rows),
            )
            db.execute(
                """
                DELETE FROM matrices AS candidate
                WHERE candidate.device_id=?
                  AND candidate.id NOT IN (
                      SELECT id FROM matrices
                      WHERE device_id=?
                      ORDER BY id DESC LIMIT ?
                  )
                  AND NOT EXISTS (
                      SELECT 1 FROM samples
                      WHERE samples.source_ref=('matrix:' || candidate.id)
                  )
                """,
                (device_id, device_id, matrix_rows),
            )

    def capture_sample(
        self,
        session_id: str,
        device_id: str,
        kind: str,
        source_ref: str,
        features: dict[str, Any],
        label: str | None = None,
        split: str = "unassigned",
    ) -> int:
        return self.execute(
            """
            INSERT INTO samples(session_id, device_id, captured_at, kind, source_ref, features_json, label, split)
            VALUES(?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (session_id, device_id, utc_now(), kind, source_ref,
             json.dumps(features, separators=(",", ":")), label, split),
        )

    @staticmethod
    def new_id(prefix: str) -> str:
        return f"{prefix}_{uuid.uuid4().hex[:16]}"


def decode_json_columns(row: dict[str, Any], *columns: str) -> dict[str, Any]:
    for column in columns:
        if column in row and isinstance(row[column], str):
            try:
                row[column.removesuffix("_json")] = json.loads(row[column])
            except json.JSONDecodeError:
                row[column.removesuffix("_json")] = None
    return row
