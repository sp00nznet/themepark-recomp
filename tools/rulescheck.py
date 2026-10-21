#!/usr/bin/env python3
"""The modern rules (docs/rules.md), each tried in the game's demo park.

    py tools/rulescheck.py [--exe build/themepark.exe]

Every scenario loads DEMO.GY as a game (0x41E035 with -2) from a new park,
then runs headless with the rule off and on:
  guards   one guard hired mid-park and 12 thugs sent in: how many get caught
  unstick  a guest put in a fenced pocket and marked stuck: does it get out
  advisor  a ride marked newly researched: does the advisor say so
  guards   (2) a guard dropped elsewhere mid-escort: is her thug let go
  entertainers  the panel's counts after 5 minutes
  handymen a handyman dropped on a ride's footprint: does he find a path
  building fences under the coaster, lamps on a path
Needs game/ (setup) and a build. Takes a few minutes.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TO_MAP = ['--click', '10:160,100', '--key', '13:14', '--key', '13.5:12', '--key', '14:1F', '--key', '14.5:14',
          '--key', '16:1C', '--key', '26:3B', '--click', '29:283,158', '--click', '32:141,39',
          '--click', '35:300,188', '--click', '38:35,154', '--click', '41:300,188', '--click', '45:300,188']
DEMO = TO_MAP + ['--call', '50:41e035:fffffffe']
BASE = 0x492824C                      # the people table in these runs
GUEST = BASE + 15 * 0xC8               # a guest of the demo park, walking at 70 s
HANDY = BASE + 241 * 0xC8              # a handyman
THUG = BASE + 332 * 0xC8               # the first thug sent in at 56 s


def dump_bytes(exe, rules, seconds, extra, at):
    path = os.path.join(HERE, 'work', 'rulescheck.bin')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    run(exe, rules, seconds, extra + ['--dump', '%s:%s' % (at, path)])
    d = open(path, 'rb').read()
    os.remove(path)
    return d


def run(exe, rules, seconds, extra, env=None):
    e = dict(os.environ, TP_RULESLOG='1', **(env or {}))
    p = subprocess.run([exe, '--headless', '--mute', '--nointro', '--lowres', '--seconds', str(seconds),
                        '--rules', rules] + DEMO + extra, cwd=HERE, capture_output=True, text=True,
                       errors='replace', env=e, timeout=1800)
    return p.stderr


def check(ok, what):
    print('  %-44s %s' % (what, 'ok' if ok else 'FAIL'))
    return ok


def guards(exe):
    waves = []
    for t in (56, 200, 400, 600):
        waves += ['--call', '%d:43c592:0' % t, '--call', '%d.1:43c592:0' % t, '--call', '%d.2:43c592:0' % t]
    extra = ['--call', '55:422232:c6d:8'] + waves
    caught = {}
    for rules in ('none', 'guards'):
        log = run(exe, rules, 900, extra, {'TP_PEOPLELOG': '25'})
        got = set(m[1] for m in re.finditer(r'\[people\] f\d+ thug (\d+) state 08', log))
        caught[rules] = len(got)
    ok = check(caught['guards'] >= 11 and caught['guards'] > caught['none'],
               'guards: thugs caught %d of 12 (without: %d)' % (caught['guards'], caught['none']))
    return ok


def unstick(exe):
    extra = ['--call', '72:408a44:%x:2d80:1880' % GUEST, '--poke', '72.1:%x=20' % (GUEST + 0xC),
             '--poke', '72.1:%x=01' % (GUEST + 0x14), '--poke', '72.1:%x=00' % (GUEST + 0x15),
             '--watch', '%x' % (GUEST + 0xC)]
    off = run(exe, 'none', 120, extra)
    on = run(exe, 'unstick', 120, extra)
    last = lambda log: (re.findall(r'watch \S+ %08X = (\w+)' % (GUEST + 0xC), log) or ['?'])[-1]
    ok = check(last(off)[-2:] == '20', 'unstick: without the rule the guest stays stuck')
    ok &= check('out of a pocket' in on and last(on)[-2:] != '20', 'unstick: with it, it walks out')
    return ok


def advisor(exe):
    extra = ['--poke', '51:4C778D=01', '--poke', '100:4BE575=03']   # advisor on; the planet rocket is new
    on = run(exe, 'advisor', 110, extra)
    return check('NEW RIDE: THE PLANET ROCKET' in on, 'advisor: tells of new research')


def escort(exe):
    # The guard catches the thug at 71.7 s with the rule; she is dropped on another tile at 71.8.
    extra = ['--call', '56:43c592:0', '--call', '60:422232:e21:8', '--call', '71.8:4248ed:c6d:14d',
             '--watch', '%x' % (THUG + 0xC)]
    on = run(exe, 'guards', 100, extra)
    return check('thug 332 let go' in on and re.search(r'%08X = 03(0[3-9A])' % (THUG + 0xC), on) is not None,
                 'guards: a thug whose guard was moved is let go')


def entertainers(exe):
    import struct
    got = {}
    for rules in ('none', 'entertainers'):
        d = dump_bytes(exe, rules, 300, [], 300)
        u32 = lambda a: struct.unpack_from('<I', d, a - 0x400000)[0]
        happy = 0
        for p in range(u32(0x4C3B10), u32(0x4C3B78), 0xC8):
            if d[p + 4 - 0x400000] == 8 and d[p + 0x59 - 0x400000] in (0, 3, 4, 5, 6, 7):
                happy += struct.unpack_from('<H', d, p + 0x42 - 0x400000)[0]
        got[rules] = happy
    return check(got['none'] == 0 and got['entertainers'] > 100,
                 'entertainers: made happy %d (without: %d)' % (got['entertainers'], got['none']))


def handymen(exe):
    extra = ['--call', '60:4248ed:16c:f1']        # dropped on (44,5), a ride's footprint
    ok = True
    for rules, want in (('none', False), ('handymen', True)):
        d = dump_bytes(exe, rules, 64, extra, 64)
        on_path = d[HANDY + 0xC - 0x400000] == 0x18
        ok &= on_path == want
    return check(ok, 'handymen: off the paths, he goes back to one')


def building(exe):
    fences = sum((['--call', '55.%d:4187d8:45:%x:1' % (k + 1, 0xA20 + k)] for k in range(5)), [])
    lamps = ['--poke', '55:4c6646=02', '--poke', '55:4c6647=00']   # the lamp is the feature picked
    lamps += sum((['--call', '55.%d:430da6:%x' % (6 + k, 0xB9F + 2 * k)] for k in range(3)), [])
    off = run(exe, 'none', 57, fences + lamps)
    on = run(exe, 'building', 57, fences + lamps)
    ok = check(off.count('call 004187d8 -> 00000000') == 5 and on.count('fence under the track') == 5,
               'building: fences go under the coaster')
    ok &= check(off.count('call 00430da6 -> 00000000') == 3 and on.count('lamp on the path') == 3,
                'building: lamps go on a path')
    # A fountain in the plaza by the gate (33,52), then bulldozed: the plaza comes back.
    import struct
    ft = ['--poke', '55:4c6646=12', '--poke', '55:4c6647=00', '--call', '55.2:430e92:d21']
    placed = dump_bytes(exe, 'building', 57, ft, 56)
    gone = dump_bytes(exe, 'building', 58, ft + ['--call', '56.5:4105d7:d21'], 57.5)
    def kind(d, x, y):
        tiles = struct.unpack_from('<I', d, 0x4C3B08 - 0x400000)[0]
        t = d[tiles + (y * 64 + x) * 4 + 2 - 0x400000]
        return struct.unpack_from('<H', d, 0x4A44D9 + t * 4 - 0x400000)[0]
    foot = [(x, y) for y in (51, 52) for x in (32, 33, 34)]
    ok &= check(all(kind(placed, x, y) == 0x8000 for x, y in foot) and all(kind(gone, x, y) == 2 for x, y in foot),
                'building: a fountain on a path, and the path back')
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(HERE, 'build', 'themepark.exe') if os.name == 'nt'
                    else os.path.join(HERE, 'build-linux', 'themepark'))
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    if not os.path.exists(os.path.join(HERE, 'game', 'THEME.CD', 'SAVE', 'DEMO.GY')) or not os.path.exists(exe):
        print('SKIP: needs game/ (with DEMO.GY) and a build at %s' % exe)
        return 0
    ok = all([guards(exe), unstick(exe), advisor(exe), escort(exe), entertainers(exe), handymen(exe), building(exe)])
    print('modern rules: %s' % ('pass' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
