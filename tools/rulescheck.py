#!/usr/bin/env python3
"""The modern rules (docs/rules.md), each tried in the game's demo park.

    py tools/rulescheck.py [--exe build/themepark.exe]

Every scenario loads DEMO.GY as a game (0x41E035 with -2) from a new park,
then runs headless with the rule off and on:
  guards   one guard hired mid-park and 12 thugs sent in: how many get caught
  unstick  a guest put in a fenced pocket and marked stuck: does it get out
  advisor  a ride marked newly researched: does the advisor say so
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
GUEST = 0x492824C + 15 * 0xC8        # a guest of the demo park, walking at 70 s


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(HERE, 'build', 'themepark.exe') if os.name == 'nt'
                    else os.path.join(HERE, 'build-linux', 'themepark'))
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    if not os.path.exists(os.path.join(HERE, 'game', 'THEME.CD', 'SAVE', 'DEMO.GY')) or not os.path.exists(exe):
        print('SKIP: needs game/ (with DEMO.GY) and a build at %s' % exe)
        return 0
    ok = all([guards(exe), unstick(exe), advisor(exe)])
    print('modern rules: %s' % ('pass' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
