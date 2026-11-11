#!/usr/bin/env python3
"""Cloud sync end to end: two RetroArch devices and one WebDAV server.

The built retroarch binary runs as device A and device B, each with its
own config, folders and keychain inside the run directory, against
webdav_server.py on 127.0.0.1. Syncs are the frontend's own: the
--cloudsync command line, the startup sync, the sync after content
closes and the menu's Sync Now (through the MENU network command).

Group 1 is cloud sync as it worked before the save hardening and must
keep working. Group 2 is what the hardening changed.
Group "finding" is behaviour found wrong on master that no commit of
the series changes.

Usage: run.py RETROARCH CORE [--group 1|2|finding|all]... [--lane NAME]...
              [--runs N] [--work DIR] [--keep] [--set KEY=VALUE]...
"""

import argparse
import os
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webdav_server import DavServer, DavState  # noqa: E402

USER     = "e2e-user"
PASSWORD = "e2e-pass-not-a-secret"
GAME     = "Game"
CORE_DIR = "cloudsync_e2e"
SRM      = "saves/%s/%s.srm" % (CORE_DIR, GAME)
AUTO     = "states/%s/%s.state.auto" % (CORE_DIR, GAME)
STATE1   = "states/%s/%s.state1" % (CORE_DIR, GAME)
SYNC_S   = 60.0
# --set KEY=VALUE: config rows every device of the run gets
CONFIG_ROWS = {}


class LaneFailed(Exception):
    pass


class LaneSkipped(Exception):
    pass


def check(cond, what):
    if not cond:
        raise LaneFailed(what)


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def get(path):
    with open(path, "rb") as f:
        return f.read()


def sram(byte):
    return bytes([byte]) * 256


class Server:
    def __init__(self, base):
        self.root = os.path.join(base, "server")
        os.makedirs(os.path.join(self.root, "ra"), exist_ok=True)
        self.state = DavState(self.root, USER, PASSWORD)
        self.srv   = DavServer(self.state).start()
        self.port  = self.srv.port

    def url(self):
        return "http://127.0.0.1:%d/ra/" % self.port

    def path(self, key):
        return os.path.join(self.root, "ra", key)

    def has(self, key):
        return os.path.isfile(self.path(key))

    def data(self, key):
        return get(self.path(key))

    def manifest(self):
        p = self.path("manifest.server")
        return get(p).decode() if os.path.isfile(p) else ""

    def backups(self, key):
        d = os.path.dirname(self.path("deleted/" + key))
        name = os.path.basename(key) + "-"
        if not os.path.isdir(d):
            return []
        return sorted(f for f in os.listdir(d) if f.startswith(name))

    def writes(self, prefix=""):
        """Requests that changed the server under ra/<prefix>."""
        return [(m, p) for (m, p, s) in self.state.requests(prefix="ra/" + prefix)
                if m in ("PUT", "DELETE", "MOVE") and 200 <= s < 300]

    def stop(self):
        self.srv.stop()


class Device:
    DIRS = ("config", "saves", "states", "system", "assets", "playlists",
            "remaps", "cache", "thumbnails", "content", "home", "logs")

    def __init__(self, base, name, server, core, retroarch):
        self.name      = name
        self.dir       = os.path.join(base, name)
        self.server    = server
        self.core      = core
        self.retroarch = retroarch
        self.proc      = None
        self.runs      = 0
        self.card      = None
        for d in self.DIRS:
            os.makedirs(os.path.join(self.dir, d), exist_ok=True)
        self.cfg      = os.path.join(self.dir, "config", "retroarch.cfg")
        self.content  = os.path.join(self.dir, "content", GAME + ".bin")
        self.core_log = os.path.join(self.dir, "logs", "core.log")
        put(self.content, b"cloudsync e2e content\n")
        self.cmd_port = free_udp_port()
        self.extra    = {}
        self.write_config()

    def p(self, *parts):
        return os.path.join(self.dir, *parts)

    def has_threads(self):
        out = subprocess.run([self.retroarch, "--features"], env=self.env(),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=30).stdout
        return b"Threading support: yes" in out

    def tasks_threaded(self):
        """Tasks run on their own thread: a build with threads and
        Threaded Tasks on."""
        rows = dict(CONFIG_ROWS, **self.extra)
        return (rows.get("threaded_data_runloop_enable") != "false"
                and self.has_threads())

    def key(self, key):
        """The local file of a sync key (saves/..., states/...)."""
        return self.p(*key.split("/"))

    def save(self):
        return self.key(SRM)

    def write_config(self, **extra):
        self.extra.update(extra)
        rows = {
            "video_driver": "null", "audio_driver": "null",
            "audio_enable": "false", "input_driver": "null",
            "microphone_enable": "false", "microphone_driver": "null",
            "input_joypad_driver": "test", "menu_driver": "rgui",
            "pause_nonactive": "false", "config_save_on_exit": "false",
            "gamemode_enable": "false",
            "confirm_quit": "false", "quit_press_twice": "false",
            "video_threaded": "false", "menu_pause_libretro": "false",
            "savestate_auto_save": "false", "savestate_auto_load": "false",
            "autosave_interval": "0", "savestate_thumbnail_enable": "false",
            "network_cmd_enable": "true",
            "network_cmd_port": str(self.cmd_port),
            "network_cmd_bind_address": "127.0.0.1",
            "savefile_directory": self.p("saves"),
            "savestate_directory": self.p("states"),
            "system_directory": self.p("system"),
            "core_assets_directory": self.p("assets"),
            "playlist_directory": self.p("playlists"),
            "input_remapping_directory": self.p("remaps"),
            "cache_directory": self.p("cache"),
            "thumbnails_directory": self.p("thumbnails"),
            "rgui_config_directory": self.p("config"),
            "log_dir": self.p("logs"),
            "cloud_sync_enable": "true", "cloud_sync_driver": "webdav",
            "cloud_sync_sync_mode": "0", "cloud_sync_destructive": "false",
            "cloud_sync_sync_saves": "true",
            "cloud_sync_sync_configs": "false",
            "cloud_sync_sync_thumbs": "false",
            "cloud_sync_sync_system": "false",
            "webdav_url": self.server.url(),
            "webdav_username": USER, "webdav_password": PASSWORD,
        }
        rows.update(CONFIG_ROWS)
        rows.update(self.extra)
        rows = {k: v for k, v in rows.items() if v is not None}
        with open(self.cfg, "w") as f:
            for k, v in rows.items():
                f.write('%s = "%s"\n' % (k, v))

    def env(self, fill=None):
        env = dict(os.environ)
        for k in ("DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"):
            env.pop(k, None)
        home = self.p("home")
        runtime = os.path.join(home, "run")
        os.makedirs(runtime, mode=0o700, exist_ok=True)
        env.update({
            "XDG_RUNTIME_DIR": runtime,
            "WAYLAND_DISPLAY": "nonexistent-wayland-socket",
            "HOME": home,
            "XDG_CONFIG_HOME": os.path.join(home, ".config"),
            "XDG_DATA_HOME": os.path.join(home, ".local", "share"),
            "XDG_CACHE_HOME": os.path.join(home, ".cache"),
            "XDG_STATE_HOME": os.path.join(home, ".local", "state"),
            "CLOUDSYNC_E2E_LOG": self.core_log,
            "CLOUDSYNC_E2E_FILL": fill or "",
            "CLOUDSYNC_E2E_CARD": self.card or "",
        })
        return env

    def _log_path(self):
        self.runs += 1
        return self.p("logs", "run%02d.log" % self.runs)

    def cli_sync(self):
        """retroarch --cloudsync: one sync, the exit code is its result."""
        log = self._log_path()
        with open(log, "w") as out:
            rc = subprocess.call([self.retroarch, "-v", "-c", self.cfg,
                                  "--cloudsync"], env=self.env(),
                                 stdout=out, stderr=subprocess.STDOUT,
                                 timeout=SYNC_S)
        self.last_log = log
        return rc, get(log).decode(errors="replace")

    def start(self, content=False, fill=None, extra_args=()):
        check(self.proc is None, "%s already running" % self.name)
        self.last_log = self._log_path()
        args = [self.retroarch, "-v", "-c", self.cfg] + list(extra_args)
        if content:
            args += ["-L", self.core, self.content]
        self.out  = open(self.last_log, "w")
        self.proc = subprocess.Popen(args, env=self.env(fill),
                                     stdout=self.out, stderr=subprocess.STDOUT)
        return self.proc.pid

    def log(self):
        return get(self.last_log).decode(errors="replace")

    def wait_log(self, pattern, timeout=SYNC_S, count=1):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if len(re.findall(pattern, self.log())) >= count:
                return True
            if self.proc is not None and self.proc.poll() is not None:
                return len(re.findall(pattern, self.log())) >= count
            time.sleep(0.05)
        return False

    def core_notes(self, what):
        if not os.path.isfile(self.core_log):
            return []
        return [l.split()[1] for l in get(self.core_log).decode().splitlines()
                if l.startswith(what + " ")]

    def wait_core(self, what, timeout=20.0, count=1):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if len(self.core_notes(what)) >= count:
                return True
            if self.proc.poll() is not None:
                break
            time.sleep(0.02)
        return False

    def cmd(self, text, timeout=5.0, reply=True):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(timeout)
        try:
            s.sendto(text.encode(), ("127.0.0.1", self.cmd_port))
            if not reply:
                return None
            data, _ = s.recvfrom(65536)
            return data.decode(errors="replace")
        except socket.timeout:
            return None
        finally:
            s.close()

    def wait_commands(self, timeout=20.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.cmd("VERSION", timeout=0.3):
                return True
            check(self.proc.poll() is None,
                  "%s exited before it took commands" % self.name)
        return False

    def sync_now(self):
        """The main menu's Sync Now, picked through the MENU command."""
        listing = self.cmd("MENU open") or ""
        for _ in range(3):
            m = re.search(r"(\d+)\D{0,8}Sync Now", listing)
            if m:
                return self.cmd("MENU a %s" % m.group(1))
            listing = self.cmd("MENU b") or ""
        raise LaneFailed("%s: no Sync Now in the menu:\n%s" % (self.name, listing))

    def stop(self, timeout=20.0):
        if self.proc is None:
            return None
        if self.proc.poll() is None:
            self.cmd("QUIT", reply=False)
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        rc = self.proc.returncode
        self.proc = None
        self.out.close()
        return rc

    def kill(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if self.proc is not None:
            self.out.close()
        self.proc = None


def synced(device):
    """Startup sync with no content: start, wait for its end, quit."""
    device.start()
    ok = device.wait_log(r"\[CloudSync\] Finished after")
    device.stop()
    check(ok, "%s: the startup sync did not finish:\n%s" % (device.name, tail(device)))


def close_sync(device, fill, extra_args=()):
    """Play the game (filling its save RAM), close it, let the sync run."""
    device.start(content=True, fill=fill, extra_args=extra_args)
    check(device.wait_core("filled"), "%s: the core never ran" % device.name)
    device.wait_log(r"\[CloudSync\] Finished after")
    check(device.wait_commands(), "%s takes no commands" % device.name)
    n = len(re.findall(r"\[CloudSync\] Finished after", device.log()))
    device.cmd("CLOSE_CONTENT")
    ok = device.wait_log(r"\[CloudSync\] Finished after", count=n + 1)
    device.stop()
    check(ok, "%s: no sync after closing content:\n%s" % (device.name, tail(device)))


def tail(device, n=25):
    return "\n".join(device.log().splitlines()[-n:])


def cli_ok(device):
    rc, log = device.cli_sync()
    check(rc == 0, "%s: --cloudsync exited %d:\n%s" % (device.name, rc,
                                                       "\n".join(log.splitlines()[-20:])))
    return log


def kept_copy(device, data, besides):
    """Some file under the device's folders, other than @besides, holds
    @data: the copy a sync makes before it replaces or removes a file."""
    for root, _, files in os.walk(device.dir):
        for f in files:
            p = os.path.join(root, f)
            if p != besides and os.path.getsize(p) == len(data) and get(p) == data:
                return p
    return None


LANES = []


def lane(group):
    def wrap(fn):
        LANES.append((group, fn.__name__, fn))
        return fn
    return wrap


def first_sync(a, b):
    put(a.save(), sram(0xA1))
    put(a.key("states/%s/%s.state1" % (CORE_DIR, GAME)), b"state one")
    cli_ok(a)
    cli_ok(b)


# ---- group 1: cloud sync as it works today --------------------------------

@lane(1)
def first_sync_both_ways(srv, a, b):
    first_sync(a, b)
    state = "states/%s/%s.state1" % (CORE_DIR, GAME)
    check(srv.data(SRM) == sram(0xA1), "server: %s is not A's save" % SRM)
    check(srv.data(state) == b"state one", "server: %s is not A's state" % state)
    check(SRM in srv.manifest() and state in srv.manifest(),
          "manifest.server lacks the keys:\n" + srv.manifest())
    check(get(b.save()) == sram(0xA1), "B did not get A's save")
    check(get(b.key(state)) == b"state one", "B did not get A's state")


@lane(1)
def edits_both_ways(srv, a, b):
    first_sync(a, b)
    close_sync(a, "A2")
    check(get(a.save()) == sram(0xA2), "A's game did not write its save")
    check(srv.data(SRM) == sram(0xA2), "A's close sync did not upload")
    check(srv.backups(SRM), "the server kept no copy of the save A replaced")
    synced(b)
    check(get(b.save()) == sram(0xA2), "B's startup sync did not fetch A's save")
    check(kept_copy(b, sram(0xA1), b.save()),
          "B kept no copy of the save the sync replaced")
    close_sync(b, "B3")
    check(srv.data(SRM) == sram(0xB3), "B's close sync did not upload")
    synced(a)
    check(get(a.save()) == sram(0xB3), "A's startup sync did not fetch B's save")


@lane(1)
def delete_moves_server_copy(srv, a, b):
    first_sync(a, b)
    os.remove(a.save())
    cli_ok(a)
    check(not srv.has(SRM), "server still has the save A deleted")
    kept = srv.backups(SRM)
    check(kept and get(os.path.join(os.path.dirname(srv.path("deleted/" + SRM)),
                                    kept[0])) == sram(0xA1),
          "the server's deleted/ holds no copy of the deleted save")
    check(re.search(r'"path": "%s",\s*"hash": ""' % re.escape(SRM), srv.manifest()),
          "manifest.server does not record the delete:\n" + srv.manifest())
    cli_ok(b)
    check(not os.path.exists(b.save()), "B kept the save A deleted")
    check(kept_copy(b, sram(0xA1), b.save()), "B kept no copy of the deleted save")


@lane(1)
def conflict_keeps_both(srv, a, b):
    first_sync(a, b)
    put(a.save(), sram(0xA2))
    put(b.save(), sram(0xB2))
    cli_ok(a)
    rc, log = b.cli_sync()
    check("Conflicting change of %s" % SRM in log, "B reported no conflict")
    check(get(b.save()) == sram(0xB2), "B's save was replaced in a conflict")
    check(srv.data(SRM) == sram(0xA2), "A's upload was replaced in a conflict")
    rc, log = b.cli_sync()
    check("Conflicting change of %s" % SRM in log,
          "the conflict was not reported again at the next sync")


@lane(1)
def ignored_files_stay_local(srv, a, b):
    a.write_config(cloud_sync_sync_configs="true")
    put(a.save(), sram(0xA1))
    ignored = ["saves/%s/%s.srm.tmp" % (CORE_DIR, GAME),
               "saves/%s/%s.srm.rafetching" % (CORE_DIR, GAME),
               "saves/%s/.DS_Store" % CORE_DIR,
               "config/content_history.lpl"]
    for key in ignored:
        put(a.key(key), b"local only")
    cli_ok(a)
    check(srv.has(SRM), "the save was not uploaded")
    for key in ignored + ["config/retroarch.cfg"]:
        check(not srv.has(key), "%s was uploaded" % key)
    check(PASSWORD.encode() not in b"".join(
        get(os.path.join(r, f)) for r, _, fs in os.walk(srv.root) for f in fs),
        "the WebDAV password reached the server")


@lane(1)
def running_save_not_replaced(srv, a, b):
    running_save(srv, a, b)


@lane(1)
def running_save_with_autosave(srv, a, b):
    if not a.has_threads():
        raise LaneSkipped("the autosave needs a build with threads")
    a.write_config(autosave_interval="1")
    running_save(srv, a, b, autosave=True)


@lane(2)
def menu_pause_writes_save(srv, a, b):
    a.write_config(menu_pause_libretro="true")
    running_save(srv, a, b, menu_pause=True)


def running_save(srv, a, b, autosave=False, menu_pause=False):
    """The sync leaves the running game's save alone: it defers it, or,
    once the game's save is on disk (autosave, or the save written when
    the menu opens), reports a conflict."""
    first_sync(a, b)
    put(b.save(), sram(0xB2))
    cli_ok(b)
    a.start(content=True, fill="A3")
    check(a.wait_core("filled"), "A's core never ran")
    check(a.core_notes("loaded")[0] == "b2" * 8,
          "A's launch sync did not hand the core B's save")
    on_disk = sram(0xB2)
    if autosave:
        deadline = time.time() + 10
        while time.time() < deadline and get(a.save()) != sram(0xA3):
            time.sleep(0.1)
        check(get(a.save()) == sram(0xA3), "the autosave never wrote the game's save")
        on_disk = sram(0xA3)
    put(b.save(), sram(0xB4))
    cli_ok(b)
    check(a.wait_commands(), "A takes no commands")
    if menu_pause:
        a.cmd("MENU open")
        deadline = time.time() + 10
        while time.time() < deadline and get(a.save()) != sram(0xA3):
            time.sleep(0.1)
        check(get(a.save()) == sram(0xA3),
              "the menu's pause did not write the game's save (%s on disk)"
              % get(a.save())[:8].hex())
        on_disk = sram(0xA3)
    a.sync_now()
    ok = a.wait_log(r"\[CloudSync\] Finished after", count=2)
    check(ok, "A's Sync Now did not finish:\n" + tail(a))
    check("Deferring \"%s\"" % SRM in a.log()
          or "Conflicting change of %s" % SRM in a.log(),
          "A's sync neither deferred the running save nor saw a conflict")
    check(get(a.save()) in (on_disk, sram(0xA3)),
          "A's running save was replaced on disk")
    a.cmd("CLOSE_CONTENT")
    a.wait_log(r"\[CloudSync\] Finished after", count=3)
    a.stop()
    check(get(a.save()) == sram(0xA3), "A's game did not write its save at close")
    check(srv.data(SRM) in (sram(0xB4), sram(0xA3)), "the server lost both saves")


@lane(1)
def destructive_mode(srv, a, b):
    a.write_config(cloud_sync_destructive="true")
    b.write_config(cloud_sync_destructive="true")
    first_sync(a, b)
    put(a.save(), sram(0xA2))
    cli_ok(a)
    check(srv.data(SRM) == sram(0xA2) and not srv.backups(SRM),
          "a destructive upload kept a server copy")
    cli_ok(b)
    check(get(b.save()) == sram(0xA2), "B did not fetch A's save")
    os.remove(a.save())
    srv.state.clear_log()
    cli_ok(a)
    check(("DELETE", "ra/" + SRM) in srv.writes(), "no DELETE: %s" % srv.writes())
    check(not srv.has(SRM) and not srv.backups(SRM), "the server kept the save")
    cli_ok(b)
    check(not os.path.exists(b.save()), "B kept the save A deleted")


@lane(1)
def wrong_password_changes_nothing(srv, a, b):
    first_sync(a, b)
    put(a.save(), sram(0xA2))
    a.write_config(webdav_password="wrong")
    srv.state.clear_log()
    rc, log = a.cli_sync()
    check(rc != 0, "the sync passed with a wrong password")
    check(not srv.writes(), "the server changed: %s" % srv.writes())
    check(get(a.save()) == sram(0xA2), "A's save changed")
    a.write_config(webdav_password=PASSWORD)
    cli_ok(a)
    check(srv.data(SRM) == sram(0xA2), "the next sync did not upload")


@lane(1)
def server_down_changes_nothing(srv, a, b):
    first_sync(a, b)
    manifest = get(a.p("assets", "manifest.local"))
    put(a.save(), sram(0xA2))
    srv.state.down = True
    rc, log = a.cli_sync()
    check(rc != 0, "the sync passed with the server down")
    check(get(a.save()) == sram(0xA2), "A's save changed")
    check(get(a.p("assets", "manifest.local")) == manifest,
          "A's local manifest changed")
    srv.state.down = False
    cli_ok(a)
    check(srv.data(SRM) == sram(0xA2), "the next sync did not upload")


@lane(1)
def error_midway_then_converges(srv, a, b):
    first_sync(a, b)
    keys = ["saves/%s/Other%d.srm" % (CORE_DIR, i) for i in range(3)]
    for i, key in enumerate(keys):
        put(a.key(key), sram(0x10 + i))
    srv.state.status[("PUT", keys[1])] = 500
    rc, log = a.cli_sync()
    check(rc != 0, "the sync passed with a failed upload")
    srv.state.reset_faults()
    cli_ok(a)
    cli_ok(b)
    for i, key in enumerate(keys):
        check(srv.has(key) and srv.data(key) == sram(0x10 + i), "server: %s" % key)
        check(get(b.key(key)) == sram(0x10 + i), "B: %s" % key)


@lane(1)
def truncated_download_kept_out(srv, a, b):
    first_sync(a, b)
    put(b.save(), sram(0xB2))
    cli_ok(b)
    srv.state.truncate[SRM] = True
    rc, log = a.cli_sync()
    check(rc != 0, "the sync passed with a cut-off download")
    check(get(a.save()) == sram(0xA1), "A's save was replaced by a cut-off download")
    srv.state.reset_faults()
    a.cli_sync()
    check(get(a.save()) in (sram(0xA1), sram(0xB2)), "A's save is neither copy")
    check(srv.data(SRM) == sram(0xB2), "the server lost B's save")


@lane(2)
def failed_fetch_retried(srv, a, b):
    """A download cut short is fetched again at the next sync, not
    reported as a conflict."""
    first_sync(a, b)
    put(b.save(), sram(0xB2))
    cli_ok(b)
    srv.state.truncate[SRM] = True
    a.cli_sync()
    srv.state.reset_faults()
    rc, log = a.cli_sync()
    check("Conflicting change of %s" % SRM not in log,
          "the retry was reported as a conflict: %s" % [
              l for l in log.splitlines() if "CloudSync" in l and SRM in l])
    check(get(a.save()) == sram(0xB2), "the next sync did not fetch the save")


@lane(1)
def server_dies_mid_sync(srv, a, b):
    first_sync(a, b)
    keys = ["saves/%s/Other%d.srm" % (CORE_DIR, i) for i in range(4)]
    for i, key in enumerate(keys):
        put(a.key(key), sram(0x20 + i))
    srv.state.clear_log()
    srv.state.die_after = 5
    rc, log = a.cli_sync()
    check(rc != 0, "the sync passed with the server gone")
    for i, key in enumerate(keys):
        check(get(a.key(key)) == sram(0x20 + i), "A lost %s" % key)
    srv.state.reset_faults()
    cli_ok(a)
    cli_ok(b)
    for i, key in enumerate(keys):
        check(get(b.key(key)) == sram(0x20 + i), "B: %s" % key)


@lane(1)
def slow_server_completes(srv, a, b):
    srv.state.delay = {"": 0.3}
    first_sync(a, b)
    check(get(b.save()) == sram(0xA1), "B did not get A's save")


@lane(1)
def state_temps_stay_local(srv, a, b):
    """The temporaries the state, auto state and thumbnail writers make
    beside the states end in .tmp and never reach the server."""
    first_sync(a, b)
    temps = ["states/%s/%s.%s" % (CORE_DIR, GAME, t)
             for t in ("state1.tmp", "state.auto.auto.tmp", "state1.ram.tmp",
                       "state1.png.tmp", "state1.png.direct.tmp")]
    for key in temps:
        put(a.key(key), b"half written")
    put(a.key(STATE1), b"state two")
    cli_ok(a)
    sent = [k for k in temps if srv.has(k)]
    check(not sent, "uploaded: %s" % ", ".join(sent))
    check(srv.data(STATE1) == b"state two", "the changed state was not uploaded")


@lane(1)
def state_saved_during_sync(srv, a, b):
    """A state saved while Sync Now runs, and the auto state written at
    close, reach the server whole; no temporary is ever sent."""
    a.write_config(savestate_auto_save="true")
    first_sync(a, b)
    a.start(content=True, fill="A5")
    check(a.wait_core("filled"), "A's core never ran")
    check(a.wait_commands(), "A takes no commands")
    srv.state.delay = {"manifest.server": 2.0}
    a.sync_now()
    a.cmd("SAVE_STATE_SLOT 1", reply=False)
    check(a.wait_log(r"\[CloudSync\] Finished after", count=2),
          "A's Sync Now did not finish:\n" + tail(a))
    srv.state.reset_faults()
    autos = len(puts(srv, AUTO))
    a.cmd("CLOSE_CONTENT")
    check(a.wait_log(r"\[CloudSync\] Finished after", count=3),
          "no sync after closing content:\n" + tail(a))
    a.stop()
    check(len(puts(srv, AUTO)) > autos,
          "the close sync did not upload the auto state")
    temps = [k for (m, k) in srv.writes("states/") if k.endswith(".tmp")]
    check(not temps, "temporaries sent: %s" % temps)
    for key in (STATE1, AUTO):
        check(os.path.isfile(a.key(key)) and get(a.key(key)) != b"state one",
              "A did not write %s" % key)
        check(srv.has(key) and srv.data(key) == get(a.key(key)),
              "the server's %s is not A's" % key)


@lane("finding")
def keychain_stays_local(srv, a, b):
    """With Sync Configs on, retroarch-keychain.cfg and its key file sit in
    the config root beside retroarch.cfg, and only retroarch.cfg is
    ignored. Joins make check once the keychain fix is in the base."""
    a.write_config(config_save_on_exit="true")
    a.start()
    check(a.wait_commands(), "A takes no commands")
    a.stop()
    a.write_config(config_save_on_exit="false", cloud_sync_sync_configs="true")
    check(os.path.isfile(a.p("config", "retroarch-keychain.key")),
          "no keychain was written")
    cli_ok(a)
    sent = [k for k in ("config/retroarch-keychain.cfg",
                        "config/retroarch-keychain.key") if srv.has(k)]
    check(not sent, "uploaded: %s" % ", ".join(sent))


# ---- group 2: what the save hardening changes ---------------------------

def server_deletes(srv, prefix="saves/"):
    return [p for (m, p) in srv.writes(prefix) if m in ("MOVE", "DELETE")]


def puts(srv, key):
    return [p for (m, p) in srv.writes(key) if m == "PUT" and p == "ra/" + key]


@lane(2)
def backups_not_uploaded(srv, a, b):
    put(a.save(), sram(0xA1))
    a.write_config(cloud_sync_sync_configs="true")
    local = ["saves/backups/cloud-sync/saves/%s/%s - 261103-120000 - cloud-sync"
             " - replaced.srm" % (CORE_DIR, GAME),
             "states/backups/%s/%s - 261103-120000 - x - before-state-load.state1"
             % (CORE_DIR, GAME),
             "config/recovery/%s - 261103-120000 - x - unsaved.srm" % GAME,
             "config/save_folders.json",
             "saves/%s/%s.srm.prev" % (CORE_DIR, GAME)]
    for key in local:
        put(a.key(key), b"device only")
    cli_ok(a)
    sent = [k for k in local if srv.has(k)]
    check(not sent, "uploaded: %s" % ", ".join(sent))


@lane(2)
def missing_folder_pauses_sync(srv, a, b):
    """The save folder sits on a card that is out at launch: its parent
    is gone, so a stand-in folder is used and the sync pauses."""
    card_saves(srv, a, b)
    os.rename(a.p("card"), a.p("card-unplugged"))
    srv.state.clear_log()
    a.start(content=True)
    check(a.wait_core("loaded"), "the game did not start")
    stand_in_session(srv, a)


@lane(2)
def missing_folder_at_runtime_load(srv, a, b):
    """As above, with the card pulled out after startup and the game
    loaded from the menu."""
    card_saves(srv, a, b)
    a.start()
    check(a.wait_commands(), "A takes no commands")
    check(a.wait_log(r"\[CloudSync\] Finished after"), "no startup sync")
    os.rename(a.p("card"), a.p("card-unplugged"))
    srv.state.clear_log()
    a.cmd("LOAD_CONTENT %s|%s" % (a.core, a.content), timeout=10)
    check(a.wait_core("loaded"), "the game did not start")
    stand_in_session(srv, a)


def card_saves(srv, a, b):
    card = a.p("card", "saves")
    a.write_config(savefile_directory=card)
    put(os.path.join(card, CORE_DIR, GAME + ".srm"), sram(0xA1))
    cli_ok(a)
    cli_ok(b)


def stand_in_session(srv, a):
    a.cmd("CLOSE_CONTENT")
    paused = a.wait_log(r"\[CloudSync\] Paused", timeout=15)
    a.stop()
    changed = srv.writes("saves/")
    check(not changed, "the sync ran from the stand-in folder: %s" % changed)
    check(not srv.state.requests(),
          "the paused sync reached the server: %s" % srv.state.requests()[:4])
    check(not os.path.exists(a.p("card")), "the folder was made again under the missing card")
    check(paused, "no paused sync after the stand-in session:\n" + tail(a))
    check("not found; saving to" in a.log(), "no stand-in folder notice")
    check(srv.data(SRM) == sram(0xA1), "the server's save changed")


@lane(2)
def unreadable_folder_stops_sync(srv, a, b):
    if os.name != "posix" or os.geteuid() == 0:
        raise LaneSkipped("needs POSIX file modes and a user they apply to")
    first_sync(a, b)
    sub = "saves/Sub/Other.srm"
    put(a.key(sub), sram(0x55))
    cli_ok(a)
    check(srv.has(sub), "setup: %s not uploaded" % sub)
    os.chmod(a.p("saves", "Sub"), 0)
    try:
        rc, log = a.cli_sync()
    finally:
        os.chmod(a.p("saves", "Sub"), stat.S_IRWXU)
    check(not server_deletes(srv), "server deletes: %s" % server_deletes(srv))
    check(rc != 0, "the sync reported success over an unreadable folder")


@lane(2)
def moved_folder_deletes_nothing(srv, a, b):
    first_sync(a, b)
    other = "saves/%s/Other.srm" % CORE_DIR
    put(a.key(other), sram(0x66))
    cli_ok(a)
    moved = a.p("saves2")
    os.makedirs(moved)
    a.write_config(savefile_directory=moved)
    srv.state.clear_log()
    cli_ok(a)
    check(not server_deletes(srv), "server deletes: %s" % server_deletes(srv))
    for key, byte in ((SRM, 0xA1), (other, 0x66)):
        p = os.path.join(moved, *key.split("/")[1:])
        check(os.path.isfile(p) and get(p) == sram(byte),
              "%s was not fetched into the new folder" % key)


@lane(2)
def mass_delete_held(srv, a, b):
    keys = ["saves/%s/Game%02d.srm" % (CORE_DIR, i) for i in range(30)]
    for i, key in enumerate(keys):
        put(a.key(key), sram(i))
    cli_ok(a)
    for key in keys[:20]:
        os.remove(a.key(key))
    srv.state.clear_log()
    a.cli_sync()
    check(not server_deletes(srv), "first sync: %d server deletes"
          % len(server_deletes(srv)))
    a.cli_sync()
    check(not server_deletes(srv), "held deletes went through at the next"
          " sync: %d" % len(server_deletes(srv)))
    back = [k for k in keys[:20] if os.path.isfile(a.key(k))]
    check(len(back) == 20, "the next sync fetched %d of the 20 back" % len(back))


@lane(2)
def sync_backups_beside_saves(srv, a, b):
    first_sync(a, b)
    put(a.save(), sram(0xA2))
    cli_ok(a)
    cli_ok(b)
    kept = kept_copy(b, sram(0xA1), b.save())
    folder = b.p("saves", "backups", "cloud-sync", "saves", CORE_DIR)
    check(kept and os.path.dirname(kept) == folder
          and re.match(re.escape(GAME)
                       + r" - \d{4}-\d\d-\d\d \d\d-\d\d-\d\d - \d{4,9}-[a-z]{4} - cloud-sync"
                       r" - replaced\.srm$",
                       os.path.basename(kept)),
          "the replaced save was kept at %s" % (kept and os.path.relpath(kept, b.dir)))


@lane(2)
def auto_state_not_replaced(srv, a, b):
    a.write_config(savestate_auto_save="true")
    put(a.save(), sram(0xA1))
    put(a.key(AUTO), b"A's auto state")
    cli_ok(a)
    cli_ok(b)
    a.start(content=True)
    check(a.wait_core("loaded"), "A's core never ran")
    put(b.key(AUTO), b"B's newer auto state")
    cli_ok(b)
    check(a.wait_commands(), "A takes no commands")
    a.sync_now()
    check(a.wait_log(r"\[CloudSync\] Finished after", count=2),
          "A's Sync Now did not finish")
    now = get(a.key(AUTO))
    a.stop()
    check(now != b"B's newer auto state",
          "the running game's auto state was replaced mid-session")


def held_launch(srv, a, b, delay=3.0, fill=None, **faults):
    """B's newer save on the server; A starts, its startup sync is slowed
    at the server manifest, and the game is launched meanwhile."""
    first_sync(a, b)
    put(b.save(), sram(0xB2))
    cli_ok(b)
    srv.state.clear_log()
    srv.state.delay = {"manifest.server": delay}
    for k, v in faults.items():
        setattr(srv.state, k, v)
    a.start(fill=fill)
    check(a.wait_commands(), "A takes no commands")
    check(not re.search(r"\[CloudSync\] Finished after", a.log()),
          "the startup sync ended before the launch")
    a.cmd("LOAD_CONTENT %s|%s" % (a.core, a.content), reply=False)


@lane(2)
def launch_waits_for_sync(srv, a, b):
    held_launch(srv, a, b)
    check(a.wait_core("loaded"), "the game did not start")
    a.wait_log(r"\[CloudSync\] Finished after")
    a.stop()
    check(a.core_notes("loaded")[0] == "b2" * 8,
          "the game started with %s, not the server's save" % a.core_notes("loaded")[0])
    check(get(a.save()) == sram(0xB2), "A's save is not the server's after the game")


@lane(2)
def launch_after_failed_sync(srv, a, b):
    held_launch(srv, a, b, status={("GET", "manifest.server"): 500})
    check(a.wait_core("loaded"), "the game did not start after the sync failed")
    check(a.wait_log(r"\[CloudSync\] Finished after"), "the sync did not end")
    check(a.core_notes("loaded")[0] == "a1" * 8, "the game did not start with A's save")
    check(a.wait_core("run", count=2), "the game stopped running")
    check(a.stop() == 0, "A did not quit cleanly")


@lane(2)
def launch_server_dies_mid_sync(srv, a, b):
    held_launch(srv, a, b, die_after=2)
    check(a.wait_core("loaded"), "the game did not start after the server died")
    check(a.core_notes("loaded")[0] == "a1" * 8, "the game did not start with A's save")
    srv.state.reset_faults()
    a.cmd("CLOSE_CONTENT")
    check(a.wait_log(r"\[CloudSync\] Finished after", count=2),
          "no sync after the game closed")
    check(a.stop() == 0, "A did not quit cleanly")


@lane(2)
def quit_while_held(srv, a, b):
    held_launch(srv, a, b, delay=5.0, fill="A9")
    time.sleep(1.5)
    check(not re.search(r"\[CloudSync\] Finished after", a.log()),
          "the sync ended before the quit")
    rc = a.stop()
    check(rc == 0, "A exited %s" % rc)
    check(get(a.save()) == sram(0xA1),
          "the save was written while held: %s" % get(a.save())[:8].hex())


@lane(2)
def cli_launch_keeps_running(srv, a, b):
    first_sync(a, b)
    put(b.save(), sram(0xB2))
    cli_ok(b)
    srv.state.delay = {"manifest.server": 3.0}
    t0 = time.time()
    a.start(content=True, fill=None)
    answered = None
    while time.time() - t0 < 20 and answered is None:
        if a.cmd("VERSION", timeout=0.2):
            answered = time.time() - t0
    synced_at = re.search(r"\[CloudSync\] Finished after", a.log())
    check(a.wait_core("loaded"), "the game did not start")
    a.stop()
    check(a.core_notes("loaded")[0] == "b2" * 8, "the game did not start with the server's save")
    check(answered is not None and not synced_at,
          "nothing answered while the launch sync ran (first answer %s s)"
          % (answered and "%.1f" % answered))


@lane(2)
def cli_launch_core_file(srv, a, b):
    """A core that reads a save file of its own at load (a memory card)
    reads the one the startup sync fetched, and its close sends no older
    card back."""
    put(a.key("saves/%s/Game.mcd" % CORE_DIR), b"\xa1" * 8)
    first_sync(a, b)
    put(b.key("saves/%s/Game.mcd" % CORE_DIR), b"\xb3" * 8)
    cli_ok(b)
    srv.state.delay = {"manifest.server": 2.0}
    a.card = a.key("saves/%s/Game.mcd" % CORE_DIR)
    a.start(content=True, fill=None)
    check(a.wait_core("loaded"), "the game did not start")
    a.cmd("CLOSE_CONTENT")
    check(a.wait_log(r"\[CloudSync\] Finished after", count=2),
          "no sync after the game closed")
    check(a.stop() == 0, "A did not quit cleanly")
    check(a.core_notes("card")[0] == "b3" * 8,
          "the game read the card it had before the sync: %s"
          % a.core_notes("card"))
    on_server = [get(os.path.join(r, f)) for r, _, fs in os.walk(srv.root)
                 for f in fs if f == "Game.mcd" and "deleted" not in r]
    check(on_server == [b"\xb3" * 8],
          "the server's card was replaced: %s" % on_server)


@lane(2)
def cli_launch_quit_while_waiting(srv, a, b):
    """Quitting while a command-line launch waits for the startup sync
    quits without starting the game."""
    first_sync(a, b)
    srv.state.delay = {"manifest.server": 5.0}
    a.card = a.key("saves/%s/Game.mcd" % CORE_DIR)
    a.start(content=True, fill=None)
    check(a.wait_commands(), "A takes no commands")
    check(not re.search(r"\[CloudSync\] Finished after", a.log()),
          "the sync ended before the quit")
    check("Waiting for cloud sync before starting the content" in a.log(),
          "no waiting notice")
    check(a.stop() == 0, "A did not quit cleanly")
    check(not a.core_notes("card"),
          "the game started as A quit: %s" % a.core_notes("card"))


@lane(2)
def not_loaded_auto_state_kept_local(srv, a, b):
    """An auto state the game could not load is moved to states/backups/
    before the next auto save: the copy stays on this device and the new
    auto state syncs as a changed file."""
    not_loaded_session(srv, a)


@lane(2)
def not_loaded_auto_state_kept_across_drives(srv, a, b):
    """As above, with states/backups on another drive: the auto state is
    held as <Game>.state.auto.not-loaded.prev and copied on a task. With
    tasks on their own thread the held file goes once the copy lands;
    without, it stays. It never reaches the server."""
    try:
        shm = tempfile.mkdtemp(prefix="cloudsync_e2e-", dir="/dev/shm")
    except OSError:
        raise LaneSkipped("needs /dev/shm")
    try:
        if os.stat(shm).st_dev == os.stat(a.p("states")).st_dev:
            raise LaneSkipped("/dev/shm is on the states' drive")
        os.symlink(shm, a.p("states", "backups"))
        not_loaded_session(srv, a, held=True)
    finally:
        shutil.rmtree(shm, ignore_errors=True)


def not_loaded_copies(a):
    return [os.path.join(r, f) for r, _, fs in os.walk(a.p("states", "backups"))
            for f in fs if f.endswith(" - not-loaded.state.auto")]


def not_loaded_session(srv, a, held=False):
    a.write_config(savestate_auto_save="true", savestate_auto_load="true")
    put(a.save(), sram(0xA1))
    put(a.key(AUTO), b"A's auto state")
    cli_ok(a)
    autos = len(puts(srv, AUTO))
    hold = a.key(AUTO) + ".not-loaded.prev"
    threaded = a.tasks_threaded()
    a.start(content=True, fill="A6")
    check(a.wait_core("filled"), "A's core never ran")
    a.wait_log(r"\[CloudSync\] Finished after")
    check(a.wait_commands(), "A takes no commands")
    a.cmd("CLOSE_CONTENT")
    ok = a.wait_log(r"\[CloudSync\] Finished after", count=2)
    deadline = time.time() + 15
    while held and threaded and os.path.exists(hold) and time.time() < deadline:
        time.sleep(0.05)
    a.stop()
    check(ok, "no sync after closing content:\n" + tail(a))
    check(any(get(f) == b"A's auto state" for f in not_loaded_copies(a)),
          "the auto state the game did not load was not kept")
    if held:
        check(os.path.exists(hold) != threaded,
              "the held auto state %s" % ("stayed after its copy landed"
                                          if threaded else "was not kept"))
    check(not os.path.exists(srv.path("states/backups")),
          "uploaded: %s" % srv.writes("states/backups/"))
    sent = [p for (_, p, _) in srv.state.requests("PUT") if p.endswith(".prev")]
    check(not sent, "uploaded: %s" % sent)
    gone = [w for w in srv.writes(AUTO) if w[0] in ("MOVE", "DELETE")
            and w[1] == "ra/" + AUTO]
    check(not gone, "the server's auto state was deleted: %s" % gone)
    check(len(puts(srv, AUTO)) > autos
          and get(a.key(AUTO)) != b"A's auto state"
          and srv.data(AUTO) == get(a.key(AUTO)),
          "the new auto state did not reach the server")


@lane(2)
def cmdline_folder_not_synced(srv, a, b):
    """A save folder given with -s belongs to that session, not to the
    main config: its syncs walk the main config's folder only."""
    first_sync(a, b)
    other = a.p("cmdline-saves")
    put(os.path.join(other, "Other.srm"), sram(0xC1))
    srv.state.clear_log()
    close_sync(a, "C2", extra_args=["-s", other])
    saved = [f for _, _, fs in os.walk(other) for f in fs if f == GAME + ".srm"]
    check(saved, "setup: the game did not save to the -s folder")
    sent = srv.writes("saves/")
    check(not sent, "the sync walked the -s folder: %s" % sent)
    check("as a first sync" not in a.log(), "the -s folder was synced as a first sync")
    check(srv.data(SRM) == sram(0xA1), "the server's save changed")


@lane(2)
def override_folder_not_synced(srv, a, b):
    """A game override's save folder belongs to that game, not to the
    main config: Sync Now while the game runs walks the main config's
    folder, and nothing from the override's is uploaded."""
    first_sync(a, b)
    other = a.p("override-saves")
    put(os.path.join(other, "Other.srm"), sram(0xC1))
    put(a.p("config", CORE_DIR, GAME + ".cfg"),
        ('savefile_directory = "%s"\n' % other).encode())
    a.start(content=True, fill="C2")
    check(a.wait_core("filled"), "A's core never ran")
    check(a.wait_log(r"\[CloudSync\] Finished after"), "no startup sync")
    check(a.wait_commands(), "A takes no commands")
    check("Game-specific overrides found" in a.log(), "setup: no game override")
    base = "saves/%s/Base.srm" % CORE_DIR
    put(a.key(base), sram(0xC3))
    srv.state.clear_log()
    a.sync_now()
    ok = a.wait_log(r"\[CloudSync\] Finished after", count=2)
    a.stop()
    check(ok, "A's Sync Now did not finish:\n" + tail(a))
    saved = [f for _, _, fs in os.walk(other) for f in fs if f == GAME + ".srm"]
    check(saved, "setup: the game did not save to the override's folder")
    sent = [w for w in srv.writes("saves/") if w != ("PUT", "ra/" + base)]
    check(not sent, "the sync walked the override's folder: %s" % sent)
    check("as a first sync" not in a.log(),
          "the override's folder was synced as a first sync")
    check(srv.has(base) and srv.data(base) == sram(0xC3),
          "the main config's folder was not synced")
    check(srv.data(SRM) == sram(0xA1), "the server's save changed")


@lane(2)
def replay_never_recorded_over(srv, a, b):
    """A replay fetched from the other device is not recorded over: with
    Replay Auto Index off the recording takes the next free slot, and
    both replays reach the server."""
    replay = "states/%s/%s.replay1" % (CORE_DIR, GAME)
    put(b.key(replay), b"B's replay")
    cli_ok(b)
    cli_ok(a)
    check(get(a.key(replay)) == b"B's replay", "setup: A did not get B's replay")
    a.write_config(replay_slot="1", replay_auto_index="false")
    a.start(content=True)
    check(a.wait_core("loaded"), "A's core never ran")
    check(a.wait_commands(), "A takes no commands")
    a.cmd("REPLAY record", reply=False)
    check(a.wait_log(r"\[Replay\] Starting movie record"),
          "no recording started:\n" + tail(a))
    check(a.wait_core("run", count=2), "the game stopped running")
    a.cmd("CLOSE_CONTENT")
    check(a.wait_log(r"\[CloudSync\] Finished after", count=2),
          "no sync after closing content:\n" + tail(a))
    a.stop()
    check(get(a.key(replay)) == b"B's replay" and srv.data(replay) == b"B's replay",
          "A recorded over B's replay")
    rec = "states/%s/%s.replay2" % (CORE_DIR, GAME)
    check(srv.has(rec) and srv.data(rec) == get(a.key(rec)),
          "A's recording did not reach the server as %s" % rec)


# ---- running ---------------------------------------------------------------

def run_lane(fn, retroarch, core, work, keep):
    base = tempfile.mkdtemp(prefix=fn.__name__ + "-", dir=work)
    srv  = Server(base)
    a    = Device(base, "A", srv, core, retroarch)
    b    = Device(base, "B", srv, core, retroarch)
    try:
        fn(srv, a, b)
        return None
    except LaneSkipped as e:
        return "skip: %s" % e
    except LaneFailed as e:
        return str(e)
    except subprocess.TimeoutExpired as e:
        return "timed out: %s" % e
    finally:
        a.kill()
        b.kill()
        srv.stop()
        if not keep:
            shutil.rmtree(base, ignore_errors=True)


def on_signal(signum, frame):
    raise SystemExit(128 + signum)


def main():
    signal.signal(signal.SIGTERM, on_signal)
    ap = argparse.ArgumentParser()
    ap.add_argument("retroarch")
    ap.add_argument("core")
    ap.add_argument("--group", action="append", default=[])
    ap.add_argument("--lane", action="append", default=[])
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--work", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    args = ap.parse_args()
    for row in args.set:
        k, _, v = row.partition("=")
        CONFIG_ROWS[k] = v

    retroarch = os.path.abspath(args.retroarch)
    core      = os.path.abspath(args.core)
    work      = args.work or tempfile.mkdtemp(prefix="cloudsync_e2e_")
    os.makedirs(work, exist_ok=True)
    groups = args.group or ["1"]
    chosen = [l for l in LANES
              if (args.lane and l[1] in args.lane)
              or (not args.lane and ("all" in groups or str(l[0]) in groups))]
    if not chosen:
        print("no such lane")
        return 2

    failed = 0
    try:
        for group, name, fn in chosen:
            for i in range(args.runs):
                why = run_lane(fn, retroarch, core, work, args.keep)
                run = " run %d/%d" % (i + 1, args.runs) if args.runs > 1 else ""
                if why is None:
                    print("[PASS] %s%s" % (name, run), flush=True)
                elif why.startswith("skip: "):
                    print("[SKIP] %s%s: %s" % (name, run, why[6:]), flush=True)
                else:
                    failed += 1
                    print("[FAIL] %s%s: %s" % (name, run, why), flush=True)
    finally:
        if not args.work and not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    print("%d lane run(s) failed" % failed if failed else "all passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
