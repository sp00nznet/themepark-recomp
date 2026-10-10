#!/usr/bin/env python3
"""Conformance: does the recompiled game still get from boot to a running park?

One scripted headless run on the virtual clock (so every run is the same run):
the title, a name typed into the prompt, the main menu, F1, the player
details, Britain on the world map, buy land, and the park's calendar running.
Each step is a milestone read from the host's --watch output on two game
variables, and a run must not fault or dispatch to code nothing lifted.

conformance.json holds the last accepted result. A run that scores lower on any
line fails (exit 1); --update accepts the current run as the new baseline. With
no game\\ or no build it prints SKIP and exits 0: the corpus is the user's own
copy of the game and is never in the repo.

    py -3 tools/conformance.py [--exe build/themepark.exe] [--update]
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE, MONTH = 0x4C6652, 0x4C4482   # attract/prompt state word; month of the park calendar (0 = Jan)

# Guest seconds; game pixels (320x200); set-1 scancodes.
SCRIPT = [
    '--click', '10:160,100',                                   # title -> name prompt
    '--key', '13:14', '--key', '13.5:12', '--key', '14:1F', '--key', '14.5:14',   # T E S T
    '--key', '16:1C',                                          # Enter -> main menu
    '--key', '26:3B',                                          # F1: setup new park
    '--click', '29:283,158',                                   # player details: tick
    '--click', '32:141,39',                                    # world map: Britain
    '--click', '35:300,188',                                   # tick
    '--click', '38:35,154',                                    # buy land
    '--click', '41:300,188', '--click', '45:300,188',          # ticks: into the park
]
SECONDS = 90


def run(exe):
    cmd = [exe, '--headless', '--seconds', str(SECONDS), '--fps', '30',
           '--watch', '%X' % STATE, '--watch', '%X' % MONTH] + SCRIPT
    p = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True, errors='replace', timeout=900)
    return p.returncode, p.stderr


def score(code, err):
    seen = {}
    for m in re.finditer(r'watch ([\d.]+)s ([0-9A-F]{8}) = ([0-9A-F]{4})', err):
        seen.setdefault(int(m.group(2), 16), []).append((float(m.group(1)), int(m.group(3), 16)))
    state = [v for _, v in seen.get(STATE, [])]
    months = [(t, v & 0xFF) for t, v in seen.get(MONTH, []) if t > 45]
    after_prompt = state[state.index(0x50) + 1:] if 0x50 in state else []
    return {
        'title': 0x66 in state,
        'name_prompt': 0x50 in state,
        'main_menu': 0 in after_prompt,
        'park_clock': len({v for _, v in months}) >= 3,      # at least two month changes
        'no_fault': 'guest fault' not in err and code == 0,
        'no_unresolved': not re.search(r'(ITAIL|ICALL): unresolved', err),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(HERE, 'build', 'themepark.exe'))
    ap.add_argument('--update', action='store_true')
    a = ap.parse_args()
    a.exe = os.path.abspath(a.exe)
    if not os.path.isdir(os.path.join(HERE, 'game', 'GAME')) or not os.path.exists(a.exe):
        print('SKIP: needs game\\ (your own copy, see README) and a build at %s' % a.exe)
        return 0
    code, err = run(a.exe)
    ms = score(code, err)
    for k, v in ms.items():
        print('  %-14s %s' % (k, 'pass' if v else 'FAIL'))
    stats = json.load(open(os.path.join(HERE, 'analysis', 'lift_stats.json')))
    now = {'milestones': sum(ms.values()), 'of': len(ms),
           'functions': stats['functions'], 'lift_errors': stats['errors']}
    print('milestones %d/%d   functions %d   lift errors %d'
          % (now['milestones'], now['of'], now['functions'], now['lift_errors']))
    path = os.path.join(HERE, 'conformance.json')
    if a.update or not os.path.exists(path):
        json.dump(now, open(path, 'w'), indent=1)
        print('baseline written to conformance.json')
        return 0 if now['milestones'] == now['of'] else 1
    base = json.load(open(path))
    worse = [k for k in ('milestones', 'functions') if now[k] < base[k]]
    if now['lift_errors'] > base['lift_errors']:
        worse.append('lift_errors')
    if worse:
        print('REGRESSION against conformance.json: %s' % ', '.join(worse))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
