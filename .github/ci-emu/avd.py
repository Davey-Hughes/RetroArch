#!/usr/bin/env python3
"""Makes the ra_dual AVD beside the others in ANDROID_AVD_HOME: API 35
x86_64 Google APIs, a 1920x1080 main display and a 1240x1080 second one
(the Thor's panels). Leaves every other AVD alone; does nothing if
ra_dual exists. RA_AVD_DATA sizes its data partition (default 6G)."""
import os
import sys

SDK = os.environ.get('ANDROID_HOME', '/home/davey/.local/share/android-sdk')
AVD_HOME = os.environ.get('ANDROID_AVD_HOME',
                          '/home/davey/.local/share/android/avd')
NAME = 'ra_dual'
IMAGE = 'system-images/android-35/google_apis/x86_64/'
# A CI runner's disk takes a smaller one
DATA = os.environ.get('RA_AVD_DATA', '6G')

CONFIG = [
    ('AvdId', NAME),
    ('PlayStore.enabled', 'false'),
    ('abi.type', 'x86_64'),
    ('avd.ini.displayname', 'RetroArch Dual'),
    ('avd.ini.encoding', 'UTF-8'),
    ('disk.dataPartition.size', DATA),
    ('fastboot.forceColdBoot', 'yes'),
    ('fastboot.forceFastBoot', 'no'),
    ('hw.accelerometer', 'yes'),
    ('hw.audioInput', 'no'),
    ('hw.battery', 'yes'),
    ('hw.camera.back', 'none'),
    ('hw.camera.front', 'none'),
    ('hw.cpu.arch', 'x86_64'),
    ('hw.cpu.ncore', '4'),
    ('hw.dPad', 'no'),
    ('hw.gps', 'no'),
    ('hw.gpu.enabled', 'yes'),
    ('hw.gpu.mode', 'swiftshader_indirect'),
    ('hw.initialOrientation', 'landscape'),
    ('hw.keyboard', 'yes'),
    ('hw.lcd.density', '320'),
    ('hw.lcd.height', '1080'),
    ('hw.lcd.width', '1920'),
    ('hw.mainKeys', 'no'),
    ('hw.ramSize', '4096'),
    ('hw.sdCard', 'no'),
    ('hw.sensors.orientation', 'yes'),
    ('hw.trackBall', 'no'),
    ('hw.display1.width', '1240'),
    ('hw.display1.height', '1080'),
    ('hw.display1.density', '320'),
    ('hw.display1.flag', '0'),
    ('hw.display1.xOffset', '-1'),
    ('hw.display1.yOffset', '-1'),
    ('image.sysdir.1', IMAGE),
    ('showDeviceFrame', 'no'),
    ('skin.dynamic', 'yes'),
    ('tag.display', 'Google APIs'),
    ('tag.id', 'google_apis'),
    ('target', 'android-35'),
    ('vm.heapSize', '576'),
]


def main():
    avd = os.path.join(AVD_HOME, NAME + '.avd')
    ini = os.path.join(AVD_HOME, NAME + '.ini')
    if os.path.exists(avd) or os.path.exists(ini):
        print('avd: %s exists, left as it is' % NAME)
        return 0
    if not os.path.exists(os.path.join(SDK, IMAGE, 'system.img')):
        sys.exit('avd: no %s in %s' % (IMAGE, SDK))
    os.makedirs(avd)
    with open(ini, 'w') as f:
        f.write('avd.ini.encoding=UTF-8\npath=%s\npath.rel=avd/%s.avd\n'
                'target=android-35\n' % (avd, NAME))
    with open(os.path.join(avd, 'config.ini'), 'w') as f:
        f.write(''.join('%s=%s\n' % kv for kv in CONFIG))
    print('avd: made %s' % avd)
    return 0


if __name__ == '__main__':
    sys.exit(main())
