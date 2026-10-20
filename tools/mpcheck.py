"""Network game check (docs/multiplayer.md): headless players on this machine.

The normal run (--players N): each player is scripted into a park through the
usual menus. The host shares its research with player 2, gives them 5000 and
grants them 20000; the last player sends thugs (Ctrl+T). Passes when every
player connects, the shared state (seed, tick, player records, both 25-park
tables) is identical on every machine just after the sync at the park gates,
a thug is in every park but the sender's, player 2's money is up by exactly
what it was given, and a ride researched by the host turns up in player 2's
park.

--rejoin: 2 players with a checkpoint every 5 s; player 2's program ends at
65 s as if it crashed, and a new one joins with nothing but a click on the
title. Passes when both say they are back in step, player 2's park is back
(with the money granted before the crash), and thugs the host sends after the
rejoin arrive.

SKIPs, like conformance.py, without game data or a build.

    py -3 tools/mpcheck.py [--players 4] [--rejoin] [--exe build/themepark.exe]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from conformance import SCRIPT                      # noqa: E402  into a park, by 45 s

B = 0x400000                                        # --dump starts at the image base
CTRL_T = ['--key', '60:1D', '--key', '60.05:14']
LOOP_RIDE = 0x4BD765                                # the loop's "available" byte (ride table)


def u32(d, a):
    return struct.unpack_from('<I', d, a - B)[0]


def region(d, name, n):
    at, size = {'seed': (0x4C3AF4, 4), 'tick': (0x4C3AFC, 4), 'players': (0x4C443C, n * 0x6C2),
                'parks': (u32(d, 0x4C3B00), 25 * 0x222), 'stock': (0x4BF1E0, 25 * 0x250)}[name]
    return d[at - B:at - B + size]


def thugs(d):
    return sum(d[a - B + 4] == 0x1A for a in range(u32(d, 0x4C3B10) + 0xC8, u32(d, 0x4C3B78), 0xC8))


def cash(d):
    return struct.unpack_from('<i', d, u32(d, 0x4C3808) + 0x0A - B)[0]


def start(exe, args, env=None):
    p = subprocess.Popen([exe, '--headless', '--fps', '30'] + args, cwd=HERE, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, text=True, errors='replace', env=env)
    time.sleep(0.5)
    return p


def check(ok, what):
    print('  %-34s %s' % (what, 'ok' if ok else 'FAIL'))
    return ok


def normal(a, tmp):
    n = a.players
    procs = []
    for k in range(n):
        net = ['--host', str(n), '--port', str(a.port)] if k == 0 else ['--join', '127.0.0.1:%d' % a.port]
        args = ['--seconds', '70'] + net + SCRIPT + ['--dump', '45.5:%s/gate%d.bin' % (tmp, k),
                                                     '--dump', '69:%s/end%d.bin' % (tmp, k)]
        if k == 0:
            args += ['--share', '46:2', '--give', '50:2:5000', '--grant', '52:2:20000',
                     '--poke', '55:%X=3' % LOOP_RIDE]
        if k == n - 1:
            args += CTRL_T
        procs.append(start(a.exe, args))
    errs = [p.communicate(timeout=900)[1] for p in procs]
    ok = True
    for k, e in enumerate(errs):
        ok &= check('player %d of %d' % (k + 1, n) in e, 'player %d connected' % (k + 1))
    if not ok:
        return False
    gate = [open('%s/gate%d.bin' % (tmp, k), 'rb').read() for k in range(n)]
    for r in ('seed', 'tick', 'players', 'parks', 'stock'):
        ok &= check(all(region(g, r, n) == region(gate[0], r, n) for g in gate[1:]), 'sync %s identical' % r)
    end = [open('%s/end%d.bin' % (tmp, k), 'rb').read() for k in range(n)]
    got = [thugs(d) for d in end]
    ok &= check(got == [1] * (n - 1) + [0], 'thugs per park %s' % got)
    # Player 2 got 5000 + 20000 from the host, who is 5000 down; the thugs
    # cost their sender 5000 a player.
    want = 30000 - (5000 * n if n == 2 else 0)
    ok &= check(cash(end[1]) - cash(end[0]) == want, 'gift and grant (player 2 %+d)' % (cash(end[1]) - cash(end[0])))
    ok &= check(end[1][LOOP_RIDE - B] & 1 == 1, 'shared research arrived')
    return ok


def rejoin(a, tmp):
    env = dict(os.environ, TP_NET_CP='150')
    host_env = dict(env, TP_NET_REJOIN='1')          # the host waits for the lost player
    states = os.path.join(os.path.dirname(a.exe), 'states')
    for f in os.listdir(states) if os.path.isdir(states) else []:
        if f.startswith('net-'):
            os.remove(os.path.join(states, f))
    host = start(a.exe, ['--seconds', '130', '--host', '2', '--port', str(a.port)] + SCRIPT +
                 ['--grant', '50:2:20000', '--key', '90:1D', '--key', '90.05:14'], host_env)
    first = start(a.exe, ['--seconds', '65', '--join', '127.0.0.1:%d' % a.port] + SCRIPT, env)
    first.communicate(timeout=900)                   # gone at 65 s, mid-game
    time.sleep(1)
    again = start(a.exe, ['--seconds', '200', '--join', '127.0.0.1:%d' % a.port, '--click', '10:160,100',
                          '--dump', '60:%s/back.bin' % tmp], env)
    e2 = again.communicate(timeout=900)[1]
    try:
        e1 = host.communicate(timeout=60)[1]
    except subprocess.TimeoutExpired:
        host.kill()
        e1 = host.communicate()[1]
    ok = check('rejoining' in e2, 'player 2 rejoined')
    ok &= check('back in step' in e1 and 'back in step' in e2, 'both back in step')
    if not os.path.exists('%s/back.bin' % tmp):
        return check(False, 'player 2 park after rejoining')
    d = open('%s/back.bin' % tmp, 'rb').read()
    ok &= check(cash(d) > 160000, 'player 2 park back (cash %d)' % cash(d))
    ok &= check(thugs(d) == 1, 'thugs after the rejoin arrived')
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--players', type=int, default=4)
    ap.add_argument('--rejoin', action='store_true')
    ap.add_argument('--port', type=int, default=7790)
    ap.add_argument('--exe', default=os.path.join(HERE, 'build', 'themepark.exe') if os.name == 'nt'
                    else os.path.join(HERE, 'build-linux', 'themepark'))
    a = ap.parse_args()
    a.exe = os.path.abspath(a.exe)
    if not os.path.isdir(os.path.join(HERE, 'game', 'GAME')) or not os.path.exists(a.exe):
        print('SKIP: needs game/ and a build at %s' % a.exe)
        return 0
    tmp = tempfile.mkdtemp()
    ok = rejoin(a, tmp) if a.rejoin else normal(a, tmp)
    shutil.rmtree(tmp, ignore_errors=True)
    print('multiplayer %s: %s' % ('rejoin' if a.rejoin else '%d players' % a.players, 'pass' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
