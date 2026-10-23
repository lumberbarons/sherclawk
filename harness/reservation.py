"""Exclusive, fault-tolerant reservations for the two OS 9 QEMU slots.

One manager installs on the host, outside every worktree, and keeps its state
in ``$OS9_HARNESS_HOME/manager.sqlite3`` behind an OS file lock.  A reservation
is a slot, a random lease token, an owner and a monotonically expiring
deadline.  Every operation re-checks the token and generation under both the
global state lock and a per-slot lock, so a stale owner cannot renew, run,
type, capture or release against a slot that has since been reassigned.

Recovery is deliberate: revoke the lease in SQLite first, collect bounded
evidence, stop and verify the exact QEMU process, reset the overlay, and only
then make the slot assignable again.  A failed recovery quarantines the slot;
restarting the manager (a new host boot or a monotonic-clock regression)
invalidates every previous lease and recovers what it can.  This is a
cooperative same-account boundary, not protection against a malicious user.

Stdlib only, and no imports from ``tools/`` or ``src/`` (ADR-0009).
"""

import contextlib
import dataclasses
import fcntl
import json
import os
import secrets
import signal
import sqlite3
import subprocess
import sys
import time

import mailbox as harness_mailbox
import provision
import qmp

LEASE_SECONDS = 600
RENEW_SECONDS = 60
LAUNCH_WINDOW = 240.0
READY_TIMEOUT = 240.0
STOP_TIMEOUT = 20.0
EVIDENCE_QMP_TIMEOUT = 3.0
LOG_TAIL_BYTES = 8192
DB_NAME = "manager.sqlite3"
SCHEMA_VERSION = "1"
SLOT_STATES = ("free", "launching", "active", "recovering", "quarantined")


class ReservationError(Exception):
    """A reservation request or guest operation could not be carried out."""


class ConfigError(ReservationError):
    """The manager is not configured for readiness or guest access."""


class NoCapacity(ReservationError):
    """Both slots are held, recovering or quarantined."""


class LeaseError(ReservationError):
    """The token, owner, generation or lease deadline no longer matches."""


class RecoveryError(ReservationError):
    """A slot could not be stopped, verified or reset."""


@dataclasses.dataclass(frozen=True)
class ProcessInfo:
    pid: int
    alive: bool
    command: str


class Clock:
    """Injectable time: expiry uses monotonic *and* wall deadlines."""

    def monotonic(self):
        return time.monotonic()

    def wall(self):
        return time.time()

    def sleep(self, seconds):
        time.sleep(seconds)


class SystemProcess:
    """Read process liveness and a command line without trusting the pidfile."""

    def info(self, pid):
        if not pid or pid <= 0:
            return ProcessInfo(pid or 0, False, "")
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return ProcessInfo(pid, False, "")
        except PermissionError:
            pass
        try:
            out = subprocess.run(
                ["/bin/ps", "-p", str(pid), "-o", "stat=,command="],
                capture_output=True, text=True, timeout=5).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return ProcessInfo(pid, True, "")
        if not out:
            return ProcessInfo(pid, False, "")
        state, _, command = out.partition(" ")
        return ProcessInfo(pid, not state.startswith("Z"), command.strip())


class Effects:
    """The host boundary: QEMU, the share, processes and QMP."""

    def __init__(self, home, share=None, clock=None, qmp_timeout=20.0):
        self.home = home
        share = share or os.environ.get("OS9_HARNESS_SHARE")
        self.share = os.path.abspath(share) if share else None
        self.clock = clock or Clock()
        self.qmp_timeout = qmp_timeout

    # ---- host identity ---------------------------------------------------

    def boot_id(self):
        """A value that changes when the host restarts, never when it sleeps."""
        if sys.platform == "darwin":
            try:
                out = subprocess.run(["sysctl", "-n", "kern.boottime"],
                                     capture_output=True, text=True, timeout=5).stdout.strip()
                return out
            except (OSError, subprocess.SubprocessError):
                return ""
        try:
            with open("/proc/sys/kernel/random/boot_id") as f:
                return f.read().strip()
        except OSError:
            return ""

    # ---- slot lifecycle --------------------------------------------------

    def pidfile_pid(self, slot):
        return provision.pid_of(slot)

    def process(self, pid):
        return SystemProcess().info(pid)

    def terminate(self, pid, timeout=STOP_TIMEOUT):
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            return True
        deadline = self.clock.monotonic() + timeout
        while self.clock.monotonic() < deadline:
            if not self.process(pid).alive:
                return True
            self.clock.sleep(0.1)
        if self.process(pid).alive:
            raise RecoveryError("pid %d ignored SIGTERM" % pid)
        return True

    def ensure_overlay(self, slot):
        if os.path.exists(slot.overlay):
            return
        if self.pidfile_pid(slot):
            raise RecoveryError("%s has a running guest but no overlay" % slot.name)
        provision.create_overlay(slot)

    def launch(self, slot):
        provision.verify_base(self.home)
        provision.verify_slot_base(slot)
        return provision.launch(slot)

    def reset(self, slot):
        """Remove the writable overlay and make a fresh one; no guest may run."""
        for path in (slot.overlay, slot.qmp, slot.pidfile):
            if os.path.isdir(path):
                raise RecoveryError("refusing to remove directory %s" % path)
            with contextlib.suppress(FileNotFoundError):
                os.remove(path)
        provision.create_overlay(slot)

    # ---- readiness and QMP ----------------------------------------------

    def mailbox(self, slot):
        if not self.share:
            raise ConfigError("set OS9_HARNESS_SHARE to the mounted Retro68 export")
        name = slot if isinstance(slot, str) else slot.name
        return os.path.join(self.share, "Harness", name)

    def wait_ready(self, slot, previous_boot=None, timeout=READY_TIMEOUT):
        name = slot if isinstance(slot, str) else slot.name
        return harness_mailbox.wait_ready(self.mailbox(name), name,
                                          previous_boot, timeout)

    def clear_stale_readiness(self, slot):
        """Remove a controller staging file left by an interrupted boot.

        Readiness is replaceable, so a stale `ready.tmp` is not evidence; the
        controller refuses to publish over it. Only call this while no guest is
        running for the slot.
        """
        path = os.path.join(self.mailbox(slot), "ready.tmp")
        try:
            os.remove(path)
        except FileNotFoundError:
            return False
        except OSError as exc:
            raise ConfigError("cannot clear stale readiness staging %s: %s"
                              % (path, exc))
        return True

    def open_qmp(self, slot, timeout=None, capture_dir=None):
        capture = qmp.Capture(capture_dir or os.path.join(self.home.path, "shots"))
        return qmp.QMP(slot.qmp, timeout=timeout or self.qmp_timeout, capture=capture)

    def qmp_alive(self, slot, timeout=2.0):
        """True only when a process answers the slot's QMP socket right now."""
        if not os.path.exists(slot.qmp):
            return False
        try:
            q = self.open_qmp(slot, timeout=timeout)
        except qmp.QMPError:
            return False
        q.close()
        return True


@dataclasses.dataclass(frozen=True)
class Lease:
    slot: str
    token: str
    owner: str
    generation: int
    expires_mono: float
    expires_wall: float
    boot: str
    boot_clock: int
    pid: int
    state: str

    def expires_in(self, mono, wall):
        return max(0.0, min(self.expires_mono - mono, self.expires_wall - wall))

    def public(self, mono, wall):
        return {
            "slot": self.slot,
            "token": self.token,
            "owner": self.owner,
            "generation": self.generation,
            "expires_in": round(self.expires_in(mono, wall), 3),
            "boot": self.boot,
            "pid": self.pid,
        }


def _row_lease(row):
    return Lease(row["slot"], row["token"], row["owner"], row["generation"],
                 row["expires_mono"], row["expires_wall"], row["boot"] or "",
                 row["boot_clock"] or 0, row["pid"] or 0, row["state"])


def _expired(lease, mono, wall):
    return mono >= lease.expires_mono or wall >= lease.expires_wall


class Manager:
    """One host-wide reservation manager over two QEMU slots."""

    def __init__(self, home=None, share=None, clock=None, effects=None,
                 ready_timeout=READY_TIMEOUT, stop_timeout=STOP_TIMEOUT,
                 renew_interval=RENEW_SECONDS):
        self.home = home if isinstance(home, provision.Home) else provision.Home(home)
        self.clock = clock or Clock()
        self.effects = effects or Effects(self.home, share=share, clock=self.clock)
        if share and not self.effects.share:
            self.effects.share = os.path.abspath(share)
        self.share = self.effects.share
        self.ready_timeout = ready_timeout
        self.stop_timeout = stop_timeout
        self.renew_interval = max(0.01, float(renew_interval))
        self.evidence_dir = os.path.join(self.home.path, "evidence")
        self.lock_dir = os.path.join(self.home.path, "locks")
        os.makedirs(self.evidence_dir, exist_ok=True)
        os.makedirs(self.lock_dir, exist_ok=True)
        self._db_path = os.path.join(self.home.path, DB_NAME)
        self._lock_path = os.path.join(self.home.path, "manager.lock")
        self._init_schema()
        self._recover_after_restart()

    # ---- storage ---------------------------------------------------------

    def _connect(self):
        conn = sqlite3.connect(self._db_path, timeout=30, isolation_level=None)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA busy_timeout=30000")
        conn.execute("PRAGMA synchronous=FULL")
        return conn

    @contextlib.contextmanager
    def _locked(self, path):
        fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            yield
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)

    def _slot_lock(self, slot_name):
        return self._locked(os.path.join(self.lock_dir, slot_name + ".lock"))

    @contextlib.contextmanager
    def _txn(self):
        with self._locked(self._lock_path):
            conn = self._connect()
            try:
                conn.execute("BEGIN IMMEDIATE")
                yield conn
                conn.execute("COMMIT")
            except BaseException:
                conn.execute("ROLLBACK")
                raise
            finally:
                conn.close()

    @contextlib.contextmanager
    def _read(self):
        with self._locked(self._lock_path):
            conn = self._connect()
            try:
                yield conn
            finally:
                conn.close()

    def _init_schema(self):
        with self._txn() as conn:
            conn.execute("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
            conn.execute("""CREATE TABLE IF NOT EXISTS slots (
                slot TEXT PRIMARY KEY,
                generation INTEGER NOT NULL DEFAULT 0,
                token TEXT,
                owner TEXT,
                state TEXT NOT NULL DEFAULT 'free',
                expires_mono REAL,
                expires_wall REAL,
                epoch TEXT,
                pid INTEGER,
                boot TEXT,
                boot_clock INTEGER,
                last_boot TEXT,
                started_wall REAL,
                updated_wall REAL,
                quarantine TEXT)""")
            conn.execute("""CREATE TABLE IF NOT EXISTS releases (
                token TEXT PRIMARY KEY,
                slot TEXT NOT NULL,
                generation INTEGER NOT NULL,
                owner TEXT,
                reason TEXT NOT NULL,
                released_wall REAL NOT NULL)""")
            for key, value in (("schema", SCHEMA_VERSION),
                               ("epoch", secrets.token_hex(16)),
                               ("host_boot", ""),
                               ("last_mono", "0")):
                conn.execute("INSERT OR IGNORE INTO meta(key, value) VALUES(?, ?)", (key, value))
            for name in provision.SLOTS:
                conn.execute("INSERT OR IGNORE INTO slots(slot) VALUES(?)", (name,))
        with contextlib.suppress(OSError):
            os.chmod(self._db_path, 0o600)

    def _meta(self, conn):
        return {r["key"]: r["value"] for r in conn.execute("SELECT key, value FROM meta")}

    def _now(self):
        return self.clock.monotonic(), self.clock.wall()

    def _set_meta(self, conn, key, value):
        conn.execute("UPDATE meta SET value=? WHERE key=?", (str(value), key))

    def _touch(self, conn):
        mono, _ = self._now()
        current = float(self._meta(conn).get("last_mono") or 0)
        if mono > current:
            self._set_meta(conn, "last_mono", repr(mono))

    def _slot(self, name):
        return provision.slot(self.home, name)

    # ---- leases ----------------------------------------------------------

    def _require_share(self):
        if not self.share:
            raise ConfigError("set OS9_HARNESS_SHARE to the mounted Retro68 export")

    def _revoke(self, conn, row, reason):
        """Invalidate a lease inside a transaction; keep evidence fields."""
        mono, wall = self._now()
        token = row["token"]
        if token:
            conn.execute("INSERT OR IGNORE INTO releases VALUES(?,?,?,?,?,?)",
                         (token, row["slot"], row["generation"], row["owner"], reason, wall))
        conn.execute("""UPDATE slots SET state='recovering', token=NULL, owner=NULL,
                        expires_mono=NULL, expires_wall=NULL, epoch=NULL,
                        quarantine=NULL, updated_wall=? WHERE slot=?""",
                     (wall, row["slot"]))
        self._touch(conn)

    def _quarantine(self, slot_name, reason):
        mono, wall = self._now()
        with self._txn() as conn:
            conn.execute("""UPDATE slots SET state='quarantined', token=NULL, owner=NULL,
                            expires_mono=NULL, expires_wall=NULL, epoch=NULL,
                            quarantine=?, updated_wall=? WHERE slot=?""",
                         (str(reason)[:400], wall, slot_name))
            self._touch(conn)

    def _recover_after_restart(self):
        restarted = False
        with self._txn() as conn:
            meta = self._meta(conn)
            boot = self.effects.boot_id()
            mono, _ = self._now()
            stored_boot = meta.get("host_boot") or ""
            last_mono = float(meta.get("last_mono") or 0)
            if stored_boot and boot and stored_boot != boot:
                restarted = True
            if last_mono and mono + 1.0 < last_mono:
                restarted = True
            self._set_meta(conn, "host_boot", boot or stored_boot)
            self._touch(conn)
        if restarted:
            self.restart(reason="manager_restart")

    def _lease_row(self, conn, token):
        return conn.execute("SELECT * FROM slots WHERE token=?", (token,)).fetchone()

    def _released(self, conn, token):
        return conn.execute("SELECT * FROM releases WHERE token=?", (token,)).fetchone()

    def _slot_for_token(self, token):
        with self._read() as conn:
            row = self._lease_row(conn, token)
            if row:
                return row["slot"]
            raise LeaseError("unknown or stale lease token")

    def _validate_locked(self, conn, token, owner):
        row = self._lease_row(conn, token)
        if row is None:
            if self._released(conn, token):
                raise LeaseError("lease was already released or revoked")
            raise LeaseError("unknown or stale lease token")
        if row["owner"] != owner:
            raise LeaseError("lease belongs to another owner")
        if row["state"] != "active":
            raise LeaseError("lease is not active (%s)" % row["state"])
        lease = _row_lease(row)
        mono, wall = self._now()
        if _expired(lease, mono, wall):
            raise LeaseError("lease expired")
        return lease

    def _expire_locked(self, conn, lease):
        row = conn.execute("SELECT * FROM slots WHERE token=?", (lease.token,)).fetchone()
        if row and row["state"] == "active":
            self._revoke(conn, row, "expired")

    def reserve(self, owner, ttl=LEASE_SECONDS):
        """Claim a free slot, boot it and return an active lease."""
        if not owner or not owner.strip():
            raise ReservationError("reserve needs a non-empty owner")
        if not 1 <= ttl <= LEASE_SECONDS:
            raise ReservationError("lease seconds must be within 1..%d" % LEASE_SECONDS)
        self._require_share()
        self._reap_expired()

        with self._txn() as conn:
            rows = list(conn.execute("SELECT * FROM slots ORDER BY slot"))
            free = [r for r in rows if r["state"] == "free"]
            if not free:
                raise NoCapacity(self._capacity_message(rows))
            row = free[0]
            generation = row["generation"] + 1
            token = secrets.token_hex(16)
            mono, wall = self._now()
            epoch = self._meta(conn)["epoch"]
            conn.execute("""UPDATE slots SET generation=?, token=?, owner=?, state='launching',
                            expires_mono=?, expires_wall=?, epoch=?, pid=NULL, boot=NULL,
                            boot_clock=NULL, started_wall=?, updated_wall=?, quarantine=NULL
                            WHERE slot=? AND state='free'""",
                         (generation, token, owner, mono + self.launch_window(),
                          wall + self.launch_window(), epoch, wall, wall, row["slot"]))
            self._touch(conn)
            last_boot = row["last_boot"]

        slot = self._slot(row["slot"])
        lease = Lease(slot.name, token, owner, generation,
                      mono + self.launch_window(), wall + self.launch_window(),
                      "", 0, 0, "launching")
        pid = 0
        try:
            with self._slot_lock(slot.name):
                if self.effects.qmp_alive(slot):
                    raise ReservationError(
                        "%s has a live QMP socket with no verified owner" % slot.name)
                self.effects.clear_stale_readiness(slot)
                self.effects.ensure_overlay(slot)
                pid = int(self.effects.launch(slot))
                info = self.effects.process(pid)
                if not info.alive:
                    raise ReservationError("launch of %s returned a dead pid" % slot.name)
                with self._txn() as conn:
                    conn.execute("UPDATE slots SET pid=?, updated_wall=? WHERE token=?",
                                 (pid, self._now()[1], token))
                    self._touch(conn)
                boot, boot_clock = self.effects.wait_ready(slot.name, last_boot,
                                                           self.ready_timeout)
                if last_boot and boot == last_boot:
                    raise ReservationError("%s did not publish a fresh boot" % slot.name)
                info = self.effects.process(pid)
                if not info.alive:
                    raise ReservationError("%s exited before readiness" % slot.name)
                mono, wall = self._now()
                with self._txn() as conn:
                    row = self._lease_row(conn, token)
                    if row is None or row["state"] != "launching":
                        raise LeaseError("reservation was revoked while booting")
                    conn.execute("""UPDATE slots SET state='active', boot=?, boot_clock=?,
                                    last_boot=?, expires_mono=?, expires_wall=?,
                                    updated_wall=? WHERE token=?""",
                                 (boot, boot_clock, boot, mono + ttl, wall + ttl, wall, token))
                    self._touch(conn)
                return Lease(slot.name, token, owner, generation, mono + ttl, wall + ttl,
                             boot, boot_clock, pid, "active")
        except BaseException as exc:
            self._fail_reservation(slot, token, lease, str(exc))
            raise

    def launch_window(self):
        return max(LAUNCH_WINDOW, self.ready_timeout + 30.0)

    def renew(self, token, owner, ttl=LEASE_SECONDS):
        """Extend an active lease; expired or stale tokens are refused."""
        if not 1 <= ttl <= LEASE_SECONDS:
            raise ReservationError("lease seconds must be within 1..%d" % LEASE_SECONDS)
        slot_name = self._slot_for_token(token)
        error = None
        lease = None
        with self._slot_lock(slot_name):
            with self._txn() as conn:
                row = self._lease_row(conn, token)
                if row is not None:
                    try:
                        lease = self._validate_locked(conn, token, owner)
                    except LeaseError as exc:
                        error = exc
                        if row["state"] == "active" and row["owner"] == owner:
                            mono, wall = self._now()
                            if _expired(_row_lease(row), mono, wall):
                                self._expire_locked(conn, _row_lease(row))
                    else:
                        mono, wall = self._now()
                        conn.execute("""UPDATE slots SET expires_mono=?, expires_wall=?,
                                        updated_wall=? WHERE token=?""",
                                     (mono + ttl, wall + ttl, wall, token))
                        lease = dataclasses.replace(lease, expires_mono=mono + ttl,
                                                    expires_wall=wall + ttl)
                        self._touch(conn)
                else:
                    error = LeaseError("unknown or stale lease token")
            if error:
                raise error
        return lease

    def release(self, token, owner):
        """Revoke, stop, verify, reset and make the slot assignable again."""
        with self._read() as conn:
            row = self._lease_row(conn, token)
            released = self._released(conn, token)
            if row is not None:
                slot_name = row["slot"]
            elif released is not None:
                slot_name = released["slot"]
            else:
                raise LeaseError("unknown or stale lease token")
        lease = None
        already = False
        with self._slot_lock(slot_name):
            with self._txn() as conn:
                row = self._lease_row(conn, token)
                if row is None:
                    released = self._released(conn, token)
                    if released:
                        already = True
                        slot_name = released["slot"]
                    else:
                        raise LeaseError("unknown or stale lease token")
                else:
                    if row["owner"] != owner:
                        raise LeaseError("lease belongs to another owner")
                    lease = _row_lease(row)
                    self._revoke(conn, row, "released")
                    self._touch(conn)
            if already:
                return {"slot": slot_name, "released": True, "already_released": True,
                        "recovered": False}
            summary = self._recover_slot(slot_name, "released", lease, lock_held=True)
            return summary

    def _reap_expired(self):
        recoverable = []
        with self._txn() as conn:
            for row in conn.execute("SELECT * FROM slots WHERE state IN ('launching','active','recovering')"):
                if row["state"] == "recovering":
                    recoverable.append((row["slot"], None, "revoked"))
                    continue
                lease = _row_lease(row)
                mono, wall = self._now()
                if _expired(lease, mono, wall):
                    recoverable.append((row["slot"], lease, "expired"))
                    self._revoke(conn, row, "expired")
            self._touch(conn)
        for slot_name, lease, reason in recoverable:
            with self._slot_lock(slot_name):
                self._recover_slot(slot_name, reason, lease, lock_held=True)

    def _capacity_message(self, rows):
        details = []
        for row in rows:
            detail = "%s=%s" % (row["slot"], row["state"])
            if row["owner"]:
                detail += " (owner %s)" % row["owner"]
            if row["quarantine"]:
                detail += " (quarantine: %s)" % row["quarantine"]
            details.append(detail)
        return "no free slot: " + ", ".join(details)

    def recover(self, slot_name=None, reason="manual"):
        """Explicit recovery; with no slot this simulates a manager restart."""
        if slot_name is None:
            return self.restart(reason=reason)
        if slot_name not in provision.SLOTS:
            raise ReservationError("unknown slot %r" % slot_name)
        with self._slot_lock(slot_name):
            lease = None
            with self._txn() as conn:
                row = conn.execute("SELECT * FROM slots WHERE slot=?", (slot_name,)).fetchone()
                if row and row["token"]:
                    lease = _row_lease(row)
                    self._revoke(conn, row, reason)
                self._touch(conn)
            return self._recover_slot(slot_name, reason, lease, lock_held=True,
                                      force_reset=True)

    def restart(self, reason="manager_restart"):
        """Invalidate every lease, then recover each slot that can be recovered."""
        with self._txn() as conn:
            epoch = secrets.token_hex(16)
            self._set_meta(conn, "epoch", epoch)
            self._set_meta(conn, "host_boot", self.effects.boot_id())
            for row in conn.execute("SELECT * FROM slots"):
                if row["state"] in ("launching", "active", "recovering") and row["token"]:
                    self._revoke(conn, row, reason)
            self._touch(conn)
        summaries = []
        for name in provision.SLOTS:
            with self._slot_lock(name):
                summary = self._recover_slot(name, reason, None, lock_held=True)
                if summary is not None:
                    summaries.append(summary)
        return {"restarted": True, "recovered": summaries}

    # ---- recovery --------------------------------------------------------

    def _recover_slot(self, slot_name, reason, lease, lock_held=False,
                      force_reset=False):
        """Stop and verify the slot's own QEMU, then reset its overlay."""
        if not lock_held:
            with self._slot_lock(slot_name):
                return self._recover_slot(slot_name, reason, lease, True, force_reset)
        slot = self._slot(slot_name)
        with self._read() as conn:
            row = conn.execute("SELECT * FROM slots WHERE slot=?", (slot_name,)).fetchone()
            meta = self._meta(conn)
        generation = lease.generation if lease else (row["generation"] if row else 0)
        owner = lease.owner if lease else (row["owner"] if row else None)
        boot = lease.boot if lease else (row["boot"] if row else "")
        last_boot = (row["last_boot"] if row else "") or boot or ""
        summary = {"slot": slot_name, "generation": generation, "owner": owner,
                   "reason": reason, "pid": 0, "stopped": False,
                   "reset": False, "quarantined": False, "evidence": []}
        try:
            if row and row["state"] == "quarantined" and not force_reset:
                summary["quarantined"] = True
                summary["skipped"] = "slot is quarantined; run recover for it"
                summary["pid"] = int(row["pid"] or 0)
                return summary
            pid = self._exact_pid(slot, lease, row)
            summary["pid"] = pid
            if not pid and self.effects.qmp_alive(slot):
                raise RecoveryError("%s answers QMP with no verified pid" % slot_name)
            if pid:
                info = self.effects.process(pid)
                if info.alive:
                    if not self._is_our_qemu(slot, info):
                        raise RecoveryError(
                            "pid %d is not %s's QEMU: %s" % (pid, slot_name, info.command[:160]))
                    path = self._safe_evidence(slot, lease, reason, "before_stop",
                                               meta, generation, owner,
                                               {"pid_command": info.command}, summary)
                    del path
                    self.effects.terminate(pid, self.stop_timeout)
                    after = self.effects.process(pid)
                    if after.alive:
                        raise RecoveryError("pid %d is still alive after SIGTERM" % pid)
                    summary["stopped"] = True
                else:
                    summary["stopped"] = True
            elif force_reset or (row and row["state"] != "free"):
                summary["stopped"] = True
            if not force_reset and not pid and row and row["state"] == "free":
                return None
            self.effects.reset(slot)
            summary["reset"] = True
            with self._txn() as conn:
                conn.execute("""UPDATE slots SET state='free', token=NULL, owner=NULL,
                                expires_mono=NULL, expires_wall=NULL, epoch=NULL, pid=NULL,
                                boot=NULL, boot_clock=NULL, last_boot=?, quarantine=NULL,
                                updated_wall=? WHERE slot=?""",
                             (last_boot, self._now()[1], slot_name))
                self._touch(conn)
        except BaseException as exc:
            detail = str(exc)
            summary["error"] = detail
            summary["quarantined"] = True
            self._quarantine(slot_name, detail)
            self._safe_evidence(slot, lease, reason, "failed", meta,
                                generation, owner, {"error": detail}, summary)
        return summary

    def _safe_evidence(self, slot, lease, reason, phase, meta, generation,
                       owner, extra, summary):
        try:
            path = self._capture_evidence(slot, lease, reason, phase, meta,
                                          generation, owner, extra)
        except Exception as exc:
            summary["evidence_error"] = str(exc)[:300]
            return None
        if path:
            summary["evidence"].append(path)
        return path

    def _exact_pid(self, slot, lease, row):
        candidates = []
        if lease and lease.pid:
            candidates.append(int(lease.pid))
        if row and row["pid"]:
            candidates.append(int(row["pid"]))
        try:
            pidfile = self.effects.pidfile_pid(slot)
        except Exception:
            pidfile = None
        if pidfile:
            candidates.append(int(pidfile))
        unique = []
        for pid in candidates:
            if pid not in unique:
                unique.append(pid)
        if len(unique) > 1:
            raise RecoveryError("lease pid and pidfile disagree for %s" % slot.name)
        return unique[0] if unique else 0

    def _is_our_qemu(self, slot, info):
        first = info.command.strip().split(" ", 1)[0]
        return (bool(first) and "qemu" in os.path.basename(first).lower()
                and slot.overlay in info.command)

    def _capture_evidence(self, slot, lease, reason, phase, meta, generation,
                          owner, extra):
        mono, wall = self._now()
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(wall))
        directory = os.path.join(self.evidence_dir, slot.name)
        os.makedirs(directory, exist_ok=True)
        base = "%s-g%d-%s-%s-%s" % (slot.name, generation, reason, phase, stamp)
        path = os.path.join(directory, base + ".json")
        data = {
            "schema": SCHEMA_VERSION,
            "slot": slot.name,
            "generation": generation,
            "owner": owner,
            "reason": reason,
            "phase": phase,
            "manager_epoch": meta.get("epoch"),
            "host_boot": meta.get("host_boot"),
            "host_monotonic": mono,
            "host_wall": wall,
            "pid": lease.pid if lease else None,
            "boot": lease.boot if lease else None,
            "qmp": slot.qmp,
            "overlay": slot.overlay,
            "log": slot.log,
            "log_tail": self._log_tail(slot.log),
        }
        data.update(extra)
        if phase == "before_stop":
            shot = os.path.join(directory, base + ".ppm")
            try:
                q = self.effects.open_qmp(slot, timeout=EVIDENCE_QMP_TIMEOUT,
                                          capture_dir=directory)
                try:
                    q.screendump(shot)
                    data["screenshot"] = shot
                finally:
                    q.close()
            except Exception as exc:
                data["screenshot_error"] = str(exc)[:300]
        index = 0
        while True:
            candidate = path if index == 0 else path[:-5] + "-%d.json" % index
            try:
                with open(candidate, "x", encoding="utf-8") as f:
                    json.dump(data, f, indent=2, sort_keys=True)
                    f.write("\n")
                return candidate
            except FileExistsError:
                index += 1

    def _log_tail(self, path):
        try:
            with open(path, "rb") as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - LOG_TAIL_BYTES))
                return f.read(LOG_TAIL_BYTES).decode("utf-8", "replace")
        except OSError:
            return ""

    def _fail_reservation(self, slot, token, lease, reason):
        """A failed boot is quarantined even when cleanup itself succeeds."""
        try:
            with self._slot_lock(slot.name):
                current = lease
                with self._txn() as conn:
                    row = self._lease_row(conn, token)
                    if row is not None:
                        current = _row_lease(row)
                        self._revoke(conn, row, "launch_failed")
                        self._touch(conn)
                with contextlib.suppress(ReservationError):
                    self._recover_slot(slot.name, "launch_failed", current,
                                       lock_held=True, force_reset=True)
        except Exception:
            pass
        finally:
            self._quarantine(slot.name, reason)

    # ---- guest operations ------------------------------------------------

    @contextlib.contextmanager
    def _leased(self, token, owner):
        """Hold the slot lock while validating, then while the caller acts."""
        slot_name = self._slot_for_token(token)
        with self._slot_lock(slot_name):
            lease = None
            error = None
            with self._read() as conn:
                try:
                    lease = self._validate_locked(conn, token, owner)
                except LeaseError as exc:
                    error = exc
            if error:
                if "expired" in str(error):
                    with self._txn() as txn:
                        row = self._lease_row(txn, token)
                        if row and row["state"] == "active":
                            self._expire_locked(txn, _row_lease(row))
                raise error
            yield lease

    def screenshot(self, token, owner, out=None):
        with self._leased(token, owner) as lease:
            slot = self._slot(lease.slot)
            if out is None:
                mono, _ = self._now()
                out = os.path.join(self.evidence_dir, lease.slot,
                                   "%s-g%d-shot-%d.ppm" % (lease.slot, lease.generation,
                                                           int(mono * 1000)))
            out = os.path.abspath(out)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            q = self.effects.open_qmp(slot)
            try:
                q.screendump(out)
            finally:
                q.close()
            return {"slot": lease.slot, "generation": lease.generation, "path": out}

    def input(self, token, owner, text=None, keys=(), click=False):
        if text is not None:
            qmp.plan_text(text)  # refuse the whole string before touching anything
        if not text and not keys and not click:
            raise ReservationError("input needs text, keys or --click")
        with self._leased(token, owner) as lease:
            slot = self._slot(lease.slot)
            q = self.effects.open_qmp(slot)
            try:
                if click:
                    q.use_relative_mouse()
                    q.click()
                if keys:
                    q.key(*keys)
                if text:
                    q.type_text(text)
            finally:
                q.close()
            return {"slot": lease.slot, "generation": lease.generation,
                    "typed": len(text or ""), "keys": len(keys), "clicked": click}

    def run(self, token, owner, argv, timeout=300):
        """Run a bounded host command under the lease, renewing every minute."""
        if not argv:
            raise ReservationError("run needs a command")
        if not 1 <= timeout <= 3600:
            raise ReservationError("command seconds must be within 1..3600")
        with self._leased(token, owner) as lease:
            slot_name = lease.slot
        env = os.environ.copy()
        env.update({
            "OS9_HARNESS_HOME": self.home.path,
            "OS9_HARNESS_SHARE": self.share or "",
            "OS9_HARNESS_SLOT": slot_name,
            "OS9_HARNESS_TOKEN": token,
            "OS9_HARNESS_OWNER": owner,
        })
        proc = subprocess.Popen(argv, env=env)
        try:
            deadline = self.clock.monotonic() + timeout
            next_renew = self.clock.monotonic() + self.renew_interval
            while True:
                remaining = deadline - self.clock.monotonic()
                if remaining <= 0:
                    raise ReservationError("command exceeded its %d second bound" % timeout)
                wait = min(remaining, max(0.01, next_renew - self.clock.monotonic()))
                try:
                    return proc.wait(timeout=wait)
                except subprocess.TimeoutExpired:
                    pass
                if self.clock.monotonic() >= deadline:
                    raise ReservationError("command exceeded its %d second bound" % timeout)
                if self.clock.monotonic() >= next_renew:
                    self.renew(token, owner)
                    next_renew = self.clock.monotonic() + self.renew_interval
        except BaseException:
            self._terminate_child(proc)
            raise

    def _terminate_child(self, proc):
        if proc.poll() is not None:
            return
        with contextlib.suppress(ProcessLookupError):
            proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            with contextlib.suppress(ProcessLookupError):
                proc.kill()
            with contextlib.suppress(subprocess.TimeoutExpired):
                proc.wait(timeout=5)

    def suite(self, token, owner, steps, stop_on_failure=True):
        """Run bounded commands in sequence under one lease and renew between them."""
        results = []
        for name, argv, timeout in steps:
            code = self.run(token, owner, argv, timeout)
            results.append({"name": name, "command": list(argv), "returncode": code})
            if code != 0 and stop_on_failure:
                break
        return {"steps": results,
                "passed": all(step["returncode"] == 0 for step in results)
                and len(results) == len(steps)}

    # ---- status ----------------------------------------------------------

    def status(self):
        with self._read() as conn:
            meta = self._meta(conn)
            rows = list(conn.execute("SELECT * FROM slots ORDER BY slot"))
        mono, wall = self._now()
        slots = []
        for row in rows:
            pid = int(row["pid"] or 0)
            try:
                pidfile = int(self.effects.pidfile_pid(self._slot(row["slot"])) or 0)
            except Exception:
                pidfile = 0
            running_pid = pid or pidfile
            alive = bool(running_pid and self.effects.process(running_pid).alive)
            lease = _row_lease(row) if row["token"] else None
            expired = bool(lease and _expired(lease, mono, wall))
            slots.append({
                "slot": row["slot"],
                "state": "expired" if expired else row["state"],
                "generation": row["generation"],
                "owner": row["owner"],
                "expires_in": round(lease.expires_in(mono, wall), 3) if lease else None,
                "boot": row["boot"],
                "pid": running_pid or None,
                "pid_alive": alive,
                "pidfile": pidfile or None,
                "untracked_pid": bool(pidfile and not pid and row["state"] == "free"),
                "quarantine": row["quarantine"],
                "overlay": self._slot(row["slot"]).overlay,
            })
        free = sum(1 for s in slots if s["state"] == "free")
        return {
            "schema": SCHEMA_VERSION,
            "manager": {
                "epoch": meta.get("epoch"),
                "host_boot": meta.get("host_boot"),
                "home": self.home.path,
                "share": self.share,
                "lease_seconds": LEASE_SECONDS,
                "capacity": len(provision.SLOTS),
                "free": free,
            },
            "slots": slots,
        }
