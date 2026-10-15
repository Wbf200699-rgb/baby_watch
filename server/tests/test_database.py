from __future__ import annotations

import sqlite3
import tempfile
import threading
import time
import unittest
from pathlib import Path

from app.db import Database, utc_now


class DatabaseConcurrencyTests(unittest.TestCase):
    def test_execute_recovers_after_transient_writer_lock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "server.sqlite3"
            database = Database(path)
            database.initialize()
            database.WRITE_RETRY_SECONDS = 1.0
            database.WRITE_ATTEMPT_SECONDS = 0.05
            writer_locked = threading.Event()

            def hold_writer_lock() -> None:
                connection = sqlite3.connect(path, timeout=0.1)
                try:
                    connection.execute("BEGIN IMMEDIATE")
                    writer_locked.set()
                    time.sleep(0.2)
                    connection.rollback()
                finally:
                    connection.close()

            thread = threading.Thread(target=hold_writer_lock)
            thread.start()
            self.assertTrue(writer_locked.wait(timeout=1.0))

            row_id = database.execute(
                "INSERT INTO devices(device_id, name, first_seen, last_seen) VALUES(?, ?, ?, ?)",
                ("dev", "dev", utc_now(), utc_now()),
            )
            thread.join(timeout=1.0)

            self.assertFalse(thread.is_alive())
            self.assertGreater(row_id, 0)
            self.assertIsNotNone(database.query_one("SELECT * FROM devices WHERE device_id='dev'"))

    def test_execute_does_not_retry_non_lock_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            database = Database(Path(directory) / "server.sqlite3")
            database.initialize()
            started_at = time.monotonic()

            with self.assertRaisesRegex(sqlite3.OperationalError, "no such table"):
                database.execute("INSERT INTO missing_table(value) VALUES(?)", (1,))

            self.assertLess(time.monotonic() - started_at, 0.5)

    def test_command_enqueue_recovers_after_transient_writer_lock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "server.sqlite3"
            database = Database(path)
            database.initialize()
            database.COMMAND_ENQUEUE_RETRY_SECONDS = 2.0
            database.COMMAND_ENQUEUE_ATTEMPT_SECONDS = 0.1
            writer_locked = threading.Event()

            def hold_writer_lock() -> None:
                connection = sqlite3.connect(path, timeout=0.1)
                try:
                    connection.execute("BEGIN IMMEDIATE")
                    writer_locked.set()
                    time.sleep(0.35)
                    connection.rollback()
                finally:
                    connection.close()

            thread = threading.Thread(target=hold_writer_lock)
            thread.start()
            self.assertTrue(writer_locked.wait(timeout=1.0))

            row, duplicate = database.enqueue_command(
                "dev",
                '{"command":"ra8.command","args":{"payload":"light_off"}}',
                utc_now(),
                "warm-light-off",
            )
            thread.join(timeout=1.0)

            self.assertFalse(thread.is_alive())
            self.assertFalse(duplicate)
            self.assertEqual(row["status"], "queued")
            self.assertEqual(row["client_request_id"], "warm-light-off")
            self.assertEqual(
                database.query_one("PRAGMA journal_mode")["journal_mode"],
                "wal",
            )


if __name__ == "__main__":
    unittest.main()
