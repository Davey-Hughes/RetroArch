#!/usr/bin/env python3
"""Save lanes against a built retroarch binary (Linux; the Windows
lanes on Windows).

Null drivers, no display, audio off, every folder RetroArch uses and
XDG_CONFIG_HOME/HOME inside a fresh run folder per run.  Build the core
and the shim first: make -C samples/runloop/save_e2e.  On Windows, the
core only: make CC=x86_64-w64-mingw32-gcc save_e2e_core.dll.

  run.py --retroarch ./retroarch --run-dir <dir> [--lane L] [--runs N]
"""
import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

try:
    import resource
except ImportError:
    resource = None

HERE = Path(__file__).resolve().parent
WINDOWS = os.name == 'nt'
SIZE = 8192
DIRS = ('saves', 'states', 'system', 'assets', 'config', 'remaps',
        'playlists', 'cache', 'screenshots', 'thumbnails', 'logs', 'home',
        'xdg-config', 'xdg-data', 'xdg-cache', 'xdg-state') + (
            ('appdata', 'localappdata') if WINDOWS else ())


def pattern(seed, size=SIZE):
    """The bytes save_e2e_core.c's fill() makes for this seed."""
    out = bytearray(size)
    s = seed
    for i in range(size):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (s >> 16) & 0xFF
    return bytes(out)


class Run:
    def __init__(self, base, name):
        self.dir = Path(base) / name
        if self.dir.exists():
            shutil.rmtree(self.dir)
        for d in DIRS:
            (self.dir / d).mkdir(parents=True)
        self.saves = self.dir / 'saves'
        self.srm = self.saves / 'game.srm'
        self.content = self.dir / 'game.bin'
        self.content.write_bytes(b'')
        self.log = self.dir / 'retroarch.log'

    def config(self, **extra):
        d = self.dir
        values = {
            'savefile_directory': d / 'saves',
            'savestate_directory': d / 'states',
            'system_directory': d / 'system',
            'core_assets_directory': d / 'assets',
            'rgui_config_directory': d / 'config',
            'input_remapping_directory': d / 'remaps',
            'playlist_directory': d / 'playlists',
            'cache_directory': d / 'cache',
            'screenshot_directory': d / 'screenshots',
            'thumbnails_directory': d / 'thumbnails',
            'log_dir': d / 'logs',
            'libretro_info_path': d / 'system',
            'video_driver': 'null', 'audio_driver': 'null',
            'audio_enable': 'false', 'microphone_enable': 'false',
            # The null joypad driver cannot start, so RetroArch falls
            # back to a real one that opens the desktop's pads.
            'input_driver': 'null', 'input_joypad_driver': 'test',
            'menu_driver': 'rgui', 'pause_nonactive': 'false',
            'config_save_on_exit': 'false', 'history_list_enable': 'false',
            'core_info_cache_enable': 'false', 'cheevos_enable': 'false',
            'cloud_sync_enable': 'false', 'network_cmd_enable': 'false',
            'confirm_quit': 'false', 'menu_show_start_screen': 'false',
            'savestate_auto_save': 'false', 'savestate_auto_load': 'false',
            'sort_savefiles_enable': 'false',
            'sort_savefiles_by_content_enable': 'false',
            'content_runtime_log': 'false',
            'content_runtime_log_aggregate': 'false',
            'autosave_interval': '1', 'save_file_compression': 'false',
        }
        values.update(extra)
        cfg = d / 'retroarch.cfg'
        cfg.write_text(''.join('%s = "%s"\n' % (k, v)
                               for k, v in values.items()))
        return cfg

    def env(self, **extra):
        env = {k: v for k, v in os.environ.items()
               if k not in ('DISPLAY', 'LD_PRELOAD')}
        d = self.dir
        env.update(WAYLAND_DISPLAY='nonexistent-wayland-socket',
                   HOME=str(d / 'home'),
                   XDG_CONFIG_HOME=str(d / 'xdg-config'),
                   XDG_DATA_HOME=str(d / 'xdg-data'),
                   XDG_CACHE_HOME=str(d / 'xdg-cache'),
                   XDG_STATE_HOME=str(d / 'xdg-state'),
                   SAVE_E2E_MARK=str(d / 'touched'))
        if WINDOWS:
            env.update(APPDATA=str(d / 'appdata'),
                       LOCALAPPDATA=str(d / 'localappdata'))
        env.update({k: str(v) for k, v in extra.items()})
        return env

    def start(self, args, cfg, frames, env, prefix=(), preexec=None,
              extra=()):
        cmd = list(prefix) + [str(args.retroarch), '--config', str(cfg),
                              '-L', str(args.core), str(self.content),
                              '--max-frames=%d' % frames, '-v'] + list(extra)
        self.proc = subprocess.Popen(cmd, cwd=self.dir, env=env,
                                     stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT,
                                     preexec_fn=preexec)
        (self.dir / 'pid').write_text('%d\n' % self.proc.pid)
        # A pipe, not a file: a file-size limit would cut the log too.
        self.drain = threading.Thread(target=self._drain)
        self.drain.start()
        return self.proc

    def _drain(self):
        with open(self.log, 'wb') as f:
            for chunk in iter(lambda: self.proc.stdout.read(4096), b''):
                f.write(chunk)

    def wait(self, timeout):
        try:
            rc = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.kill()
            rc = None
        self.drain.join()
        return rc

    def kill(self):
        """SIGKILL (TerminateProcess) our own process, nothing else."""
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()

    def temps(self):
        return sorted(p.name for p in self.saves.iterdir()
                      if p.name.endswith('.tmp'))

    def copies(self, reason, folder=None):
        """Safety copies or recovery files of game.srm with @reason."""
        d = folder or self.saves / 'backups'
        if not d.is_dir():
            return []
        return sorted(p for p in d.iterdir()
                      if p.name.startswith('game - ')
                      and p.name.endswith(' - %s.srm' % reason))

    def wait_for(self, path, timeout=30):
        deadline = time.time() + timeout
        while not path.exists() and self.proc.poll() is None \
                and time.time() < deadline:
            time.sleep(0.01)
        return path.exists()


def check(failures, cond, what):
    if not cond:
        failures.append(what)


def no_core_writes_limit():
    # ulimit -f 4 with SIGXFSZ ignored: writes past 4096 bytes fail.
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))


def lane_autosave_full_disk(args, n):
    """An autosave on a full disk keeps the old save whole."""
    r = Run(args.run_dir, 'autosave_full_disk-%d' % n)
    old = pattern(11)
    r.srm.write_bytes(old)
    cfg = r.config()
    env = r.env(SAVE_E2E_TOUCH_FRAME=30, SAVE_E2E_SEED=12,
                SAVE_E2E_FRAME_MS=10)
    r.start(args, cfg, 400, env, preexec=no_core_writes_limit)
    rc = r.wait(60)
    data = r.srm.read_bytes() if r.srm.exists() else b''
    failures = []
    check(failures, (r.dir / 'touched').exists(), 'core never touched save RAM')
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, len(data) == SIZE,
          '.srm is %d bytes, not %d' % (len(data), SIZE))
    check(failures, data == old, '.srm bytes changed')
    check(failures, not r.temps(), 'temporaries left: %s' % r.temps())
    # The whole process is out of room: no recovery file fits either.
    log = r.log.read_text(errors='replace')
    left = r.copies('unsaved') + r.copies('unsaved', r.dir / 'config'
                                           / 'recovery')
    check(failures, log.count("Couldn't save %s." % r.srm) == 1,
          'not one write-failed notice at close')
    check(failures, not left, 'recovery files left: %s' % left)
    return r, failures


def lane_autosave_kill_mid_write(args, n):
    """Killed in the middle of an autosave write, the save is
    the old image or the new one, never a part."""
    r = Run(args.run_dir, 'autosave_kill_mid_write-%d' % n)
    old, new = pattern(21), pattern(22)
    r.srm.write_bytes(old)
    cfg = r.config()
    mark = r.dir / 'stalled'
    env = r.env(SAVE_E2E_TOUCH_FRAME=30, SAVE_E2E_SEED=22,
                SAVE_E2E_FRAME_MS=10, LD_PRELOAD=args.shim,
                SAVE_E2E_STALL=r.srm, SAVE_E2E_STALL_MARK=mark)
    r.start(args, cfg, 100000, env)
    deadline = time.time() + 30
    while not mark.exists() and r.proc.poll() is None \
            and time.time() < deadline:
        time.sleep(0.01)
    stalled = mark.exists()
    r.kill()
    r.drain.join()
    data = r.srm.read_bytes() if r.srm.exists() else b''
    failures = []
    check(failures, stalled, 'no write stalled (worker never wrote)')
    check(failures, data in (old, new),
          '.srm is %d bytes and neither image' % len(data))
    return r, failures


def lane_no_rewrite_at_launch(args, n):
    """An unchanged raw save is never written, compression on."""
    r = Run(args.run_dir, 'no_rewrite_at_launch-%d' % n)
    old = pattern(31)
    r.srm.write_bytes(old)
    cfg = r.config(save_file_compression='true')
    env = r.env(SAVE_E2E_FRAME_MS=10)
    trace = r.dir / 'strace.log'
    prefix = ['strace', '-f', '-qq', '-o', str(trace),
              '-e', 'trace=open,openat,creat,rename,renameat,renameat2']
    r.start(args, cfg, args.frames or 3000, env, prefix=prefix)
    rc = r.wait(120)
    srm = str(r.srm)
    writes = [l for l in trace.read_text(errors='replace').splitlines()
              if srm in l and ('O_WRONLY' in l or 'O_RDWR' in l
                               or 'rename' in l or 'creat(' in l)]
    failures = []
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, not writes, '%d writes: %s' % (len(writes),
                                                   writes[:2]))
    check(failures, r.srm.read_bytes() == old, '.srm bytes changed')
    return r, failures


def lane_unreadable_backed_up(args, n):
    """A save that cannot be read at load is copied as it is
    before the first write over it; one notice."""
    r = Run(args.run_dir, 'unreadable_backed_up-%d' % n)
    old, new = pattern(51), pattern(52)
    r.srm.write_bytes(old)
    os.chmod(r.srm, 0)
    cfg = r.config()
    env = r.env(SAVE_E2E_TOUCH_FRAME=30, SAVE_E2E_SEED=52,
                SAVE_E2E_FRAME_MS=10)
    r.start(args, cfg, 300, env)
    # The read error passes once the game runs (a share back online).
    touched = r.wait_for(r.dir / 'touched')
    os.chmod(r.srm, 0o644)
    rc = r.wait(60)
    log = r.log.read_text(errors='replace')
    copies = r.copies('unreadable')
    failures = []
    check(failures, touched, 'core never touched save RAM')
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, len(copies) == 1, '%d unreadable copies' % len(copies))
    check(failures, copies and copies[0].read_bytes() == old,
          'the copy is not the old save')
    check(failures, r.srm.read_bytes() == new, '.srm not written')
    check(failures, log.count("Couldn't read %s;" % r.srm) == 1,
          'not one unreadable notice in the log')
    return r, failures


def lane_late_core_backed_up(args, n):
    """A core that reports save RAM only after the load: the old
    save is copied before the close writes over it; no notice."""
    r = Run(args.run_dir, 'late_core_backed_up-%d' % n)
    old, new = pattern(41), pattern(42)
    r.srm.write_bytes(old)
    cfg = r.config()
    env = r.env(SAVE_E2E_LATE=5, SAVE_E2E_TOUCH_FRAME=30,
                SAVE_E2E_SEED=42, SAVE_E2E_FRAME_MS=10)
    r.start(args, cfg, 100, env)
    rc = r.wait(60)
    log = r.log.read_text(errors='replace')
    copies = r.copies('not-loaded')
    failures = []
    check(failures, (r.dir / 'touched').exists(), 'core never touched save RAM')
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, len(copies) == 1, '%d not-loaded copies' % len(copies))
    check(failures, copies and copies[0].read_bytes() == old,
          'the copy is not the old save')
    check(failures, 'save_e2e reported no memory for "%s"' % r.srm in log,
          'the log does not name the core')
    check(failures, "Couldn't read" not in log, 'a notice for a late core')
    check(failures, log.count('Backed up "%s" (not-loaded)' % r.srm) == 1,
          'the copy is not logged once')
    check(failures, r.srm.read_bytes() == new, 'the close did not write')
    return r, failures


def lane_noload_save_backed_up(args, n):
    """--sram-mode noload-save: the save is not loaded, so it is copied
    before the first write over it."""
    r = Run(args.run_dir, 'noload_save_backed_up-%d' % n)
    old, new = pattern(81), pattern(82)
    r.srm.write_bytes(old)
    cfg = r.config()
    env = r.env(SAVE_E2E_TOUCH_FRAME=30, SAVE_E2E_SEED=82,
                SAVE_E2E_FRAME_MS=10)
    r.start(args, cfg, 300, env, extra=('--sram-mode', 'noload-save'))
    rc = r.wait(60)
    copies = r.copies('not-loaded')
    failures = []
    check(failures, (r.dir / 'touched').exists(), 'core never touched save RAM')
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, len(copies) == 1, '%d not-loaded copies' % len(copies))
    check(failures, copies and copies[0].read_bytes() == old,
          'the copy is not the old save')
    check(failures, r.srm.read_bytes() == new, '.srm not written')
    return r, failures


def lane_explicit_save_copies(args, n):
    """The network command's SAVE_FILES on a save that owes a copy (it
    did not read at load): the copy, then the write, neither synced
    (the close writes those bytes again, synced). No autosave worker
    runs."""
    r = Run(args.run_dir, 'explicit_save_copies-%d' % n)
    old, new = pattern(91), pattern(92)
    r.srm.write_bytes(old)
    os.chmod(r.srm, 0)
    port = 55500 + n
    cfg = r.config(autosave_interval='0', network_cmd_enable='true',
                   network_cmd_port=str(port))
    env = r.env(SAVE_E2E_TOUCH_FRAME=30, SAVE_E2E_SEED=92,
                SAVE_E2E_FRAME_MS=10)
    trace = r.dir / 'strace.log'
    prefix = ['strace', '-f', '-qq', '-y', '-ttt', '-o', str(trace),
              '-e', 'trace=fsync,fdatasync']
    r.start(args, cfg, 600, env, prefix=prefix)
    touched = r.wait_for(r.dir / 'touched')
    os.chmod(r.srm, 0o644)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    deadline = time.time() + 20
    while r.srm.read_bytes() != new and r.proc.poll() is None \
            and time.time() < deadline:
        sock.sendto(b'SAVE_FILES', ('127.0.0.1', port))
        time.sleep(0.5)
    written = time.time()
    sock.close()
    rc = r.wait(60)
    copies = r.copies('unreadable')
    syncs = [l for l in trace.read_text(errors='replace').splitlines()
             if str(r.saves) in l and float(l.split()[1]) < written]
    failures = []
    check(failures, touched, 'core never touched save RAM')
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, len(copies) == 1, '%d unreadable copies' % len(copies))
    check(failures, copies and copies[0].read_bytes() == old,
          'the copy is not the old save')
    check(failures, r.srm.read_bytes() == new, '.srm not written')
    check(failures, not syncs, '%d syncs under saves/: %s' % (len(syncs),
                                                             syncs[:2]))
    return r, failures


def lane_cut_copy_old_core(args, n):
    """A save longer than the core's memory (an old core's partial
    load) is copied before the first shorter write over it."""
    r = Run(args.run_dir, 'cut_copy_old_core-%d' % n)
    old, new = pattern(61), pattern(62, 2048)
    r.srm.write_bytes(old)
    cfg = r.config()
    env = r.env(SAVE_E2E_SRAM=2048, SAVE_E2E_TOUCH_FRAME=30,
                SAVE_E2E_SEED=62, SAVE_E2E_FRAME_MS=10)
    r.start(args, cfg, 300, env)
    rc = r.wait(60)
    copies = r.copies('cut')
    failures = []
    check(failures, (r.dir / 'touched').exists(), 'core never touched save RAM')
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, len(copies) == 1, '%d cut copies' % len(copies))
    check(failures, copies and copies[0].read_bytes() == old,
          'the copy is not the old save')
    check(failures, r.srm.read_bytes() == new, '.srm not written')
    return r, failures


def lane_state_load_copy(args, n):
    """Hotkey state loads (the network command) of an older
    state.  The autosave rests 60 s, so only the flush before each load
    writes the progress; the next write backs it up, once per load."""
    r = Run(args.run_dir, 'state_load_copy-%d' % n)
    port = 55600 + n
    cfg = r.config(autosave_interval='60', network_cmd_enable='true',
                   network_cmd_port=str(port),
                   sort_savestates_enable='false',
                   sort_savestates_by_content_enable='false')
    env = r.env(SAVE_E2E_TOUCHES='30:61,300:62,700:63',
                SAVE_E2E_FRAME_MS=10)
    r.start(args, cfg, 1200, env)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    failures = []
    state = r.dir / 'states' / 'game.state'
    mark = r.dir / 'touched'
    check(failures, r.wait_for(Path('%s-61' % mark)), 'no first touch')
    sock.sendto(b'SAVE_STATE', ('127.0.0.1', port))
    check(failures, r.wait_for(state, 10)
          and not Path('%s-62' % mark).exists(),
          'state not saved before the second touch')
    for seed in (62, 63):
        check(failures, r.wait_for(Path('%s-%d' % (mark, seed))),
              'no touch %d' % seed)
        sock.sendto(b'LOAD_STATE', ('127.0.0.1', port))
        time.sleep(1.0)
    sock.close()
    rc = r.wait(60)
    copies = r.copies('before-state-load')
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, len(copies) == 2,
          '%d before-state-load copies' % len(copies))
    check(failures, [c.read_bytes() for c in copies]
          == [pattern(62), pattern(63)],
          'the copies are not the progress before each load')
    check(failures, r.srm.read_bytes() == pattern(61),
          '.srm is not the loaded state')
    return r, failures


def lane_flush_points(args, n):
    """Pausing and opening the menu write a changed save at once
    (the autosave rests 60 s); pausing again unchanged writes nothing.
    Each write is a rename, so a new inode counts one.  Network commands
    are not read while the menu is open, so the menu comes last."""
    r = Run(args.run_dir, 'flush_points-%d' % n)
    port = 55900 + n
    cfg = r.config(autosave_interval='60', network_cmd_enable='true',
                   network_cmd_port=str(port))
    env = r.env(SAVE_E2E_TOUCHES='30:81,300:82', SAVE_E2E_FRAME_MS=10)
    # Ended by kill: the close's own write must not come first.
    r.start(args, cfg, 20000, env)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    failures = []
    mark = r.dir / 'touched'

    def send(cmd, settle=0.0):
        sock.sendto(cmd, ('127.0.0.1', port))
        time.sleep(settle)

    def written(image, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline and r.proc.poll() is None:
            if r.srm.exists() and r.srm.read_bytes() == image:
                return True
            time.sleep(0.02)
        return False

    def inode():
        return r.srm.stat().st_ino if r.srm.exists() else None

    check(failures, r.wait_for(Path('%s-81' % mark)), 'no first touch')
    send(b'PAUSE_TOGGLE')
    check(failures, written(pattern(81)), 'pause: the changed save not written')
    send(b'PAUSE_TOGGLE', 0.5)
    before = inode()
    send(b'PAUSE_TOGGLE', 1.0)
    check(failures, inode() == before, 'pause: an unchanged save written')
    send(b'PAUSE_TOGGLE')
    check(failures, r.wait_for(Path('%s-82' % mark)), 'no second touch')
    send(b'MENU_TOGGLE')
    check(failures, written(pattern(82)),
          'menu open: the changed save not written')
    sock.close()
    r.kill()
    r.drain.join()
    return r, failures


def lane_changed_elsewhere(args, n):
    """A second RetroArch writes the save while the first runs.
    The first's next write (its worker, or its close without one) backs
    up the second's save first; its own writes never do."""
    r = Run(args.run_dir, 'changed_elsewhere-%d' % n)
    other = Run(r.dir, 'other')
    cfg = r.config()
    cfg2 = other.config(savefile_directory=r.saves, autosave_interval='0')
    r.start(args, cfg, 1500, r.env(SAVE_E2E_TOUCHES='30:71,150:72,1000:74',
                                   SAVE_E2E_FRAME_MS=10))
    failures = []
    mark = r.dir / 'touched'
    check(failures, r.wait_for(Path('%s-72' % mark)), 'no touch 72')
    deadline = time.time() + 5
    while not (r.srm.exists() and r.srm.read_bytes() == pattern(72)) \
            and time.time() < deadline:
        time.sleep(0.05)
    worker = r.srm.exists() and r.srm.read_bytes() == pattern(72)
    other.start(args, cfg2, 100, other.env(SAVE_E2E_TOUCHES='30:73',
                                           SAVE_E2E_FRAME_MS=10))
    rc2 = other.wait(60)
    check(failures, r.srm.read_bytes() == pattern(73),
          "the other's save not written")
    check(failures, not Path('%s-74' % mark).exists(),
          'touch 74 came before the other wrote')
    rc = r.wait(60)
    copies = r.copies('changed-elsewhere')
    log = r.log.read_text(errors='replace')
    check(failures, rc == 0 and rc2 == 0, 'exits %s %s' % (rc, rc2))
    check(failures, len(copies) == 1,
          '%d changed-elsewhere copies' % len(copies))
    check(failures, copies and copies[0].read_bytes() == pattern(73),
          "the copy is not the other's save")
    backups = r.saves / 'backups'
    extra = sorted(p.name for p in backups.iterdir()
                   if p not in copies) if backups.is_dir() else []
    check(failures, not extra, 'other copies or recovery files %s' % extra)
    check(failures, r.srm.read_bytes() == pattern(74), '.srm not written')
    check(failures, log.count('changed outside RetroArch') == (0 if worker
                                                                else 1),
          'changed-elsewhere log lines')
    check(failures, 'changed outside RetroArch' not in
          other.log.read_text(errors='replace'), 'the other told')
    return r, failures


def lane_recovery_announced(args, n):
    """A recovery file newer than the save is announced at load; after
    the game wrote its save it is not."""
    r = Run(args.run_dir, 'recovery_announced-%d' % n)
    r.srm.write_bytes(pattern(71))
    rec = r.saves / 'backups' / 'game - 2026-01-01 00-00-00 - save_e2e - unsaved.srm'
    rec.parent.mkdir()
    rec.write_bytes(pattern(72))
    old = time.time() - 120
    os.utime(r.srm, (old, old))
    cfg = r.config()
    notice = 'Unsaved progress for %s is in Save Backups.' % r.srm
    failures = []
    for i, seed in enumerate((73, 74)):
        env = r.env(SAVE_E2E_TOUCH_FRAME=30, SAVE_E2E_SEED=seed,
                    SAVE_E2E_FRAME_MS=10)
        r.start(args, cfg, 100, env)
        rc = r.wait(60)
        log = r.log.read_text(errors='replace')
        check(failures, rc == 0, 'run %d: retroarch exit %s' % (i, rc))
        check(failures, log.count(notice) == (1, 0)[i],
              'run %d: %d notices' % (i, log.count(notice)))
        check(failures, r.srm.read_bytes() == pattern(seed),
              'run %d: the save was not written' % i)
    return r, failures


def lane_nosave_netplay_disconnect(args, n):
    """--sram-mode load-nosave: a netplay host stopped in the game (the
    hotkey's network command) starts no autosave, so a later change in
    save RAM never reaches the save."""
    r = Run(args.run_dir, 'nosave_netplay_disconnect-%d' % n)
    old = pattern(101)
    r.srm.write_bytes(old)
    then = time.time() - 120
    os.utime(r.srm, (then, then))
    mtime = r.srm.stat().st_mtime_ns
    port, host_port = 55700 + n, 55800 + n
    cfg = r.config(network_cmd_enable='true', network_cmd_port=str(port),
                   netplay_public_announce='false',
                   netplay_use_mitm_server='false',
                   netplay_nat_traversal='false',
                   core_info_savestate_bypass='true')
    # Frame 30's touch writes the bytes already there: a mark only.
    env = r.env(SAVE_E2E_TOUCHES='30:101,500:102', SAVE_E2E_FRAME_MS=10)
    r.start(args, cfg, 800, env, extra=('--sram-mode', 'load-nosave',
                                        '--host', '--port', str(host_port)))
    failures = []
    mark = r.dir / 'touched'
    check(failures, r.wait_for(Path('%s-101' % mark)), 'no first touch')
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(b'NETPLAY_HOST_TOGGLE', ('127.0.0.1', port))
    sock.close()
    # Stopped once the host's port refuses a connection (Windows takes
    # about a second to refuse).
    stopped = False
    deadline = time.time() + 10
    while not stopped and r.proc.poll() is None and time.time() < deadline:
        time.sleep(0.5)
        try:
            socket.create_connection(('127.0.0.1', host_port), 3).close()
        except ConnectionRefusedError:
            stopped = True
        except OSError:
            pass
    check(failures, stopped and not Path('%s-102' % mark).exists(),
          'the host did not stop before the second touch')
    touched = r.wait_for(Path('%s-102' % mark))
    rc = r.wait(60)
    log = r.log.read_text(errors='replace')
    check(failures, 'You have joined as player 1' in log, 'never hosted')
    check(failures, touched, 'no second touch')
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, r.srm.read_bytes() == old, '.srm bytes changed')
    check(failures, r.srm.stat().st_mtime_ns == mtime, '.srm mtime changed')
    check(failures, not r.temps(), 'temporaries left: %s' % r.temps())
    return r, failures


def lane_windows_locked_close(args, n):
    """Windows: a .srm held open without FILE_SHARE_DELETE at
    close keeps its bytes; the image goes to one recovery file."""
    r = Run(args.run_dir, 'windows_locked_close-%d' % n)
    old, new = pattern(61), pattern(62)
    r.srm.write_bytes(old)
    # A Windows build may not log to a pipe; it logs to logs/ too.
    cfg = r.config(log_to_file='true', log_to_file_timestamp='false')
    env = r.env(SAVE_E2E_TOUCH_FRAME=30, SAVE_E2E_SEED=62,
                SAVE_E2E_FRAME_MS=10)
    # Python opens files sharing read and write, not delete.
    held = open(r.srm, 'rb')
    try:
        r.start(args, cfg, 300, env)
        rc = r.wait(120)
    finally:
        held.close()
    logs = [r.log] + sorted((r.dir / 'logs').glob('*.log'))
    log = max((p.read_text(errors='replace') for p in logs if p.exists()),
              key=lambda t: t.count("Couldn't save"))
    recs = r.copies('unsaved')
    notices = [l for l in log.splitlines() if "Couldn't save" in l
               and 'your progress is in Save Backups' in l]
    failures = []
    check(failures, (r.dir / 'touched').exists(), 'core never touched save RAM')
    check(failures, rc == 0, 'retroarch exit %s' % rc)
    check(failures, r.srm.read_bytes() == old, '.srm changed')
    check(failures, len(recs) == 1, '%d recovery files' % len(recs))
    check(failures, recs and recs[0].read_bytes() == new,
          'the recovery file is not the image')
    check(failures, len(notices) == 1, '%d recovery notices' % len(notices))
    return r, failures


LANES = {
    'autosave_full_disk': lane_autosave_full_disk,
    'autosave_kill_mid_write': lane_autosave_kill_mid_write,
    'no_rewrite_at_launch': lane_no_rewrite_at_launch,
    'unreadable_backed_up': lane_unreadable_backed_up,
    'late_core_backed_up': lane_late_core_backed_up,
    'noload_save_backed_up': lane_noload_save_backed_up,
    'explicit_save_copies': lane_explicit_save_copies,
    'cut_copy_old_core': lane_cut_copy_old_core,
    'recovery_announced': lane_recovery_announced,
    'state_load_copy': lane_state_load_copy,
    'changed_elsewhere': lane_changed_elsewhere,
    'flush_points': lane_flush_points,
    'nosave_netplay_disconnect': lane_nosave_netplay_disconnect,
    'windows_locked_close': lane_windows_locked_close,
}
# Lanes that need Windows, and the ones Windows cannot run.
WINDOWS_LANES = {'windows_locked_close'}
POSIX_LANES = {'autosave_full_disk', 'autosave_kill_mid_write',
               'no_rewrite_at_launch', 'unreadable_backed_up',
               'explicit_save_copies'}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--retroarch', required=True, type=Path)
    p.add_argument('--run-dir', required=True, type=Path)
    p.add_argument('--core', type=Path, default=HERE / (
        'save_e2e_core.dll' if WINDOWS else 'save_e2e_core.so'))
    p.add_argument('--shim', type=Path, default=HERE / 'stall_shim.so')
    p.add_argument('--lane', action='append', choices=sorted(LANES))
    p.add_argument('--runs', type=int, default=1)
    p.add_argument('--frames', type=int, default=0,
                   help='no_rewrite_at_launch frames (10 ms each)')
    p.add_argument('--keep', action='store_true',
                   help='keep passing run folders')
    args = p.parse_args()
    args.retroarch = args.retroarch.resolve()
    args.core = args.core.resolve()
    args.shim = args.shim.resolve()
    args.run_dir = args.run_dir.resolve()
    lanes = [l for l in args.lane or sorted(LANES)
             if l not in (POSIX_LANES if WINDOWS else WINDOWS_LANES)]
    need = [args.retroarch, args.core]
    if 'autosave_kill_mid_write' in lanes:
        need.append(args.shim)
    for f in need:
        if not f.exists():
            p.error('%s not found' % f)
    bad = 0
    for lane in lanes:
        for n in range(args.runs):
            r, failures = LANES[lane](args, n)
            if failures:
                bad += 1
                print('FAIL %s run %d: %s (%s)' % (lane, n,
                                                  '; '.join(failures), r.dir))
            else:
                print('pass %s run %d' % (lane, n))
                if not args.keep:
                    shutil.rmtree(r.dir)
    print('%d of %d runs failed' % (bad, len(lanes) * args.runs))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
