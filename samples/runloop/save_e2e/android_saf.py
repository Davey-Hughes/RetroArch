#!/usr/bin/env python3
"""Saves and states in a Storage Access Framework folder, on a device.

Lanes:
  saf_rename               autosave on, a state save, a quit: the .srm is
                           whole, no .tmp or .prev is left, the state file
                           exists; then a force-stop mid-session leaves a
                           whole .srm.
  saf_stale_aside          autosave off, a stale "<game>.srm.prev" there:
                           the close write replaces the .srm through
                           "<game>.srm.2.prev", the stale file keeps its
                           bytes and no aside of the write is left.
  saf_rename_replace_fail  every aside name, "<game>.srm.prev" to
                           "<game>.srm.99.prev", taken, so the replace
                           cannot be made: the .srm keeps its old bytes
                           and the failed write is logged.

Run by hand, with the device owner's go-ahead: it starts and stops the
package given.  It refuses to start while that package is running.
Nothing of the user's is changed: RetroArch runs on a config written
for this run (CONFIGFILE extra), and every run saves into a new
"run-<time>" folder under the SAF folder given.

Before the first run, in the build under test, pick the SAF folder once
through Settings > Directory > Save Files (the system folder picker), so
the package holds the grant, and copy the "saf://" value RetroArch
wrote for it.
"""

import argparse
import subprocess
import sys
import time

ACTIVITY = "com.retroarch.browser.retroactivity.RetroActivityFuture"


class Device:
    def __init__(self, adb, serial):
        self.base = [adb, "-s", serial]

    def sh(self, cmd, check=True):
        r = subprocess.run(self.base + ["shell", cmd], capture_output=True,
                           text=True, timeout=120)
        if check and r.returncode != 0:
            raise RuntimeError("adb shell %r: %s" % (cmd, r.stderr.strip()))
        return r.stdout

    def push_text(self, text, remote):
        r = subprocess.run(self.base + ["shell", "cat > '%s'" % remote],
                           input=text, text=True, capture_output=True,
                           timeout=60)
        if r.returncode != 0:
            raise RuntimeError("write %s: %s" % (remote, r.stderr.strip()))


def running(dev, pkg):
    return dev.sh("pidof %s" % pkg, check=False).strip() != ""


def command(dev, cmd):
    # toybox nc waits for a reply that never comes
    dev.sh("echo -n '%s' | timeout 2 nc -u -w1 127.0.0.1 55355" % cmd,
           check=False)


def md5(dev, path):
    out = dev.sh("md5sum '%s' 2>/dev/null" % path, check=False).split()
    return out[0] if out else None


def listing(dev, folder):
    return dev.sh("ls -1 '%s' 2>/dev/null" % folder, check=False).split()


def write_config(dev, args, cfg, saf_run, interval):
    text = "\n".join([
        'savefile_directory = "%s/saves"' % saf_run,
        'savestate_directory = "%s/states"' % saf_run,
        'sort_savefiles_enable = "false"',
        'sort_savestates_enable = "false"',
        'autosave_interval = "%d"' % interval,
        'savestate_auto_save = "false"',
        'savestate_auto_load = "false"',
        'savestate_auto_index = "false"',
        'save_file_compression = "false"',
        'savestate_file_compression = "false"',
        'network_cmd_enable = "true"',
        'network_cmd_port = "55355"',
        'quit_press_twice = "false"',
        'confirm_quit = "false"',
        'config_save_on_exit = "false"',
        'audio_enable = "false"',
        'audio_driver = "null"',
        'pause_nonactive = "false"',
        'log_verbosity = "true"',
        ""])
    dev.push_text(text, cfg)


def launch(dev, args, cfg):
    dev.sh("logcat -c", check=False)
    dev.sh("am start -n %s/%s -e ROM '%s' -e LIBRETRO '%s' -e CONFIGFILE '%s'"
           % (args.package, ACTIVITY, args.content, args.core, cfg))
    time.sleep(10)
    if not running(dev, args.package):
        raise RuntimeError("the package did not start")


def quit_and_wait(dev, pkg):
    command(dev, "QUIT")
    for _ in range(30):
        if not running(dev, pkg):
            return True
        time.sleep(1)
    return False


def log_count(dev, needle):
    return dev.sh("logcat -d", check=False).count(needle)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--adb", required=True)
    ap.add_argument("--serial", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--saf-folder", required=True,
                    help='the "saf://" value of the picked folder')
    ap.add_argument("--fs-folder", required=True,
                    help="the same folder as a path adb can read, e.g. "
                         "/storage/4A21-0000/ra-save-e2e")
    ap.add_argument("--core", required=True, help="core .so on the device")
    ap.add_argument("--content", required=True,
                    help="content on the device whose battery save RAM "
                         "changes while it runs (a test ROM that counts)")
    ap.add_argument("--interval", type=int, default=2)
    args = ap.parse_args()

    dev = Device(args.adb, args.serial)
    if running(dev, args.package):
        print("BLOCKED: %s is running; ask before stopping it" % args.package)
        return 2

    stamp = time.strftime("run-%Y%m%d-%H%M%S")
    saf_run = "%s/%s" % (args.saf_folder, stamp)
    fs_run = "%s/%s" % (args.fs_folder, stamp)
    saves, states = fs_run + "/saves", fs_run + "/states"
    game = args.content.rsplit("/", 1)[-1].rsplit(".", 1)[0]
    srm = "%s/%s.srm" % (saves, game)
    cfg = "/sdcard/Android/data/%s/files/save_e2e-%s.cfg" % (args.package,
                                                             stamp)
    failures = []

    def check(name, ok):
        print("  %-60s %s" % (name, "ok" if ok else "FAIL"))
        if not ok:
            failures.append(name)

    print("saf_rename")
    write_config(dev, args, cfg, saf_run, args.interval)
    launch(dev, args, cfg)
    time.sleep(args.interval * 4)
    command(dev, "SAVE_STATE")
    time.sleep(3)
    check("quit ends the process", quit_and_wait(dev, args.package))
    names = listing(dev, saves)
    check(".srm written", game + ".srm" in names)
    check("no .tmp or .prev left in saves",
          not [n for n in names if n.endswith((".tmp", ".prev"))])
    snames = listing(dev, states)
    check("state file written", game + ".state" in snames)
    check("no .tmp left in states",
          not [n for n in snames if n.endswith(".tmp")])
    check("no failed save logged", log_count(dev, "Failed to save SRAM") == 0)
    size = dev.sh("stat -c %%s '%s' 2>/dev/null" % srm, check=False).strip()

    launch(dev, args, cfg)
    time.sleep(args.interval * 3 + 1)
    dev.sh("am force-stop %s" % args.package)
    after = dev.sh("stat -c %%s '%s' 2>/dev/null" % srm, check=False).strip()
    check("force-stop leaves a whole .srm", size != "" and after == size)
    check("force-stop leaves no .tmp in saves",
          not [n for n in listing(dev, saves) if n.endswith(".tmp")])

    print("saf_stale_aside")
    write_config(dev, args, cfg, saf_run, 0)
    dev.push_text("stale aside\n", srm + ".prev")
    stale = md5(dev, srm + ".prev")
    before = md5(dev, srm)
    launch(dev, args, cfg)
    time.sleep(5)
    check("quit ends the process", quit_and_wait(dev, args.package))
    check(".srm replaced", before is not None and md5(dev, srm) not in (None, before))
    check("the stale aside keeps its bytes", md5(dev, srm + ".prev") == stale)
    names = listing(dev, saves)
    check("no aside of the write left",
          not [n for n in names if n.endswith(".prev") and n != game + ".srm.prev"])
    check("no failed save logged", log_count(dev, "Failed to save SRAM") == 0)

    print("saf_rename_replace_fail")
    before = md5(dev, srm)
    dev.sh("for n in $(seq 2 99); do echo taken > '%s'.$n.prev; done" % srm)
    launch(dev, args, cfg)
    time.sleep(5)
    check("quit ends the process", quit_and_wait(dev, args.package))
    check(".srm keeps its old bytes", before is not None and md5(dev, srm) == before)
    check("the failed write is logged",
          log_count(dev, "Failed to save SRAM") >= 1)
    check("no .tmp left in saves",
          not [n for n in listing(dev, saves) if n.endswith(".tmp")])

    print("left on the device for inspection: %s and %s" % (fs_run, cfg))
    if failures:
        print("android_saf: %d FAIL" % len(failures))
        return 1
    print("android_saf: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
