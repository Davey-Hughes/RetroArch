#!/usr/bin/env python3
"""Saves and states in a Storage Access Framework folder, on a device.

Lanes:
  saf_rename               autosave on, a state save, a quit: the .srm is
                           whole, no .tmp or .prev is left, the state file
                           exists; then a force-stop mid-session leaves a
                           whole .srm.
  saf_stale_aside          autosave off, a stale "<game>.srm.prev" left by
                           a killed replace, and "<game>.srm.3.prev" to
                           "<game>.srm.99.prev" taken: the close write
                           replaces the .srm through "<game>.srm.2.prev",
                           the only free aside name, every taken name keeps
                           its bytes and no aside of the write is left.
  saf_rename_replace_fail  "<game>.srm.2.prev" taken too, so every aside
                           name is: the .srm keeps its old bytes and the
                           failed write is logged.
  saf_aside_folder         autosave off, in a folder of its own, a folder
                           at "<game>.srm.prev": the close write replaces
                           the .srm through "<game>.srm.2.prev", the
                           folder and the file in it stay as they were.
  saf_long_name            autosave off, in a folder of its own, a copy of
                           the content whose save name is 251 bytes, too
                           long for any ".prev" aside: the second close
                           write replaces the .srm through "<name>.old"
                           and leaves none; with a file at "<name>.old",
                           the third keeps the .srm and that file as they
                           were and logs the failed write.
  saf_root_tree            autosave off, in a tree picked at a volume root
                           (--saf-root): the first close writes the .srm,
                           the second replaces it, nothing is left beside
                           it and no failed write is logged.  Skipped
                           without --saf-root; not every device lets a
                           volume root be picked.

Run by hand, with the device owner's go-ahead: it starts and stops the
package given.  It refuses to start while that package is running.
Nothing of the user's is changed: RetroArch runs on a config written
for this run (CONFIGFILE extra), and every run saves into a new
"run-<time>" folder under the SAF folder given.

Before the first run, in the build under test, pick the SAF folder once
through Settings > Directory > Save Files (the system folder picker), so
the package holds the grant, and copy the "saf://" value RetroArch
wrote for it.  For saf_root_tree, do the same with a volume's root.
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


def asides_md5(dev, srm, first):
    out = dev.sh("for n in $(seq %d 99); do cat '%s'.$n.prev || echo gone; "
                 "done | md5sum" % (first, srm), check=False).split()
    return out[0] if out else None


def is_dir(dev, path):
    out = dev.sh("[ -d '%s' ] && echo dir" % path, check=False)
    return out.strip() == "dir"


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
        'history_list_enable = "false"',
        'content_runtime_log = "false"',
        'content_runtime_log_aggregate = "false"',
        'audio_enable = "false"',
        'audio_driver = "null"',
        'pause_nonactive = "false"',
        'log_verbosity = "true"',
        ""])
    dev.push_text(text, cfg)


def launch(dev, args, cfg, content=None):
    dev.sh("logcat -c", check=False)
    dev.sh("am start -n %s/%s -e ROM '%s' -e LIBRETRO '%s' -e CONFIGFILE '%s'"
           % (args.package, ACTIVITY, content or args.content, args.core, cfg))
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
    ap.add_argument("--saf-root",
                    help='the "saf://" value of a volume root picked the '
                         'same way, for saf_root_tree')
    ap.add_argument("--fs-root",
                    help="that root as a path adb can read, e.g. "
                         "/storage/4A21-0000")
    ap.add_argument("--interval", type=int, default=2)
    args = ap.parse_args()
    if (args.saf_root is None) != (args.fs_root is None):
        ap.error("--saf-root and --fs-root go together")

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
    left = [fs_run]

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
    dev.sh("for n in $(seq 3 99); do echo taken $n > '%s'.$n.prev; done" % srm)
    stale = md5(dev, srm + ".prev")
    taken = asides_md5(dev, srm, 3)
    before = md5(dev, srm)
    launch(dev, args, cfg)
    time.sleep(5)
    check("quit ends the process", quit_and_wait(dev, args.package))
    check(".srm replaced", before is not None and md5(dev, srm) not in (None, before))
    check("the stale aside keeps its bytes", md5(dev, srm + ".prev") == stale)
    check("every taken aside name keeps its bytes",
          asides_md5(dev, srm, 3) == taken)
    check("no .2.prev left", md5(dev, srm + ".2.prev") is None)
    check("no failed save logged", log_count(dev, "Failed to save SRAM") == 0)

    print("saf_rename_replace_fail")
    before = md5(dev, srm)
    dev.sh("echo taken 2 > '%s.2.prev'" % srm)
    launch(dev, args, cfg)
    time.sleep(5)
    check("quit ends the process", quit_and_wait(dev, args.package))
    check(".srm keeps its old bytes", before is not None and md5(dev, srm) == before)
    check("the failed write is logged",
          log_count(dev, "Failed to save SRAM") >= 1)
    check("no .tmp left in saves",
          not [n for n in listing(dev, saves) if n.endswith(".tmp")])

    print("saf_aside_folder")
    folder_srm = "%s/folder/saves/%s.srm" % (fs_run, game)
    write_config(dev, args, cfg, saf_run + "/folder", 0)
    launch(dev, args, cfg)
    time.sleep(5)
    check("quit ends the process", quit_and_wait(dev, args.package))
    dev.sh("mkdir '%s.prev' && echo inside > '%s.prev/keep'"
           % (folder_srm, folder_srm))
    before = md5(dev, folder_srm)
    inside = md5(dev, folder_srm + ".prev/keep")
    launch(dev, args, cfg)
    time.sleep(5)
    check("quit ends the process", quit_and_wait(dev, args.package))
    check(".srm replaced",
          before is not None and md5(dev, folder_srm) not in (None, before))
    check("the folder at .prev and its file stay as they were",
          is_dir(dev, folder_srm + ".prev") and inside is not None
          and md5(dev, folder_srm + ".prev/keep") == inside)
    check("no .2.prev left", md5(dev, folder_srm + ".2.prev") is None)
    check("no failed save logged", log_count(dev, "Failed to save SRAM") == 0)

    print("saf_long_name")
    long_game = ("long-name-" + "x" * 247)[:247]
    assert len((long_game + ".srm").encode()) == 251
    ext = args.content.rsplit("/", 1)[-1].rsplit(".", 1)[1:]
    long_content = "/sdcard/Android/data/%s/files/%s" % (
        args.package, ".".join([long_game] + ext))
    long_srm = "%s/long/saves/%s.srm" % (fs_run, long_game)
    dev.sh("cp '%s' '%s'" % (args.content, long_content))
    left.append(long_content)
    write_config(dev, args, cfg, saf_run + "/long", 0)
    launch(dev, args, cfg, long_content)
    time.sleep(5)
    check("quit ends the process", quit_and_wait(dev, args.package))
    before = md5(dev, long_srm)
    check(".srm written", before is not None)
    launch(dev, args, cfg, long_content)
    time.sleep(5)
    check("quit ends the process", quit_and_wait(dev, args.package))
    check(".srm replaced", md5(dev, long_srm) not in (None, before))
    check("no .old or .tmp left",
          md5(dev, long_srm + ".old") is None
          and md5(dev, long_srm + ".tmp") is None)
    check("no failed save logged", log_count(dev, "Failed to save SRAM") == 0)
    dev.push_text("taken old\n", long_srm + ".old")
    before = md5(dev, long_srm)
    taken = md5(dev, long_srm + ".old")
    launch(dev, args, cfg, long_content)
    time.sleep(5)
    check("quit ends the process", quit_and_wait(dev, args.package))
    check(".srm keeps its old bytes",
          before is not None and md5(dev, long_srm) == before)
    check("the file at .old keeps its bytes",
          taken is not None and md5(dev, long_srm + ".old") == taken)
    check("the failed write is logged",
          log_count(dev, "Failed to save SRAM") >= 1)

    if args.saf_root:
        print("saf_root_tree")
        root_saves = "%s/%s/saves" % (args.fs_root, stamp)
        root_srm = "%s/%s.srm" % (root_saves, game)
        write_config(dev, args, cfg, "%s/%s" % (args.saf_root, stamp), 0)
        launch(dev, args, cfg)
        time.sleep(5)
        check("quit ends the process", quit_and_wait(dev, args.package))
        first = md5(dev, root_srm)
        check(".srm written", first is not None)
        check("no failed save logged", log_count(dev, "Failed to save SRAM") == 0)
        launch(dev, args, cfg)
        time.sleep(5)
        check("quit ends the process", quit_and_wait(dev, args.package))
        check(".srm replaced", md5(dev, root_srm) not in (None, first))
        check("no .tmp or .prev left in saves",
              not [n for n in listing(dev, root_saves)
                   if n.endswith((".tmp", ".prev"))])
        check("no failed save logged", log_count(dev, "Failed to save SRAM") == 0)
        left.append("%s/%s" % (args.fs_root, stamp))
    else:
        print("saf_root_tree: skipped (no --saf-root)")

    print("left on the device for inspection: %s" % ", ".join(left + [cfg]))
    if failures:
        print("android_saf: %d FAIL" % len(failures))
        return 1
    print("android_saf: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
