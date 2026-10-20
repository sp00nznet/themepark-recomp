"""Network game check (docs/multiplayer.md): N headless players on this machine.

Each player is scripted into a park through the usual menus; the last one
sends thugs (Ctrl+T) at 60 s. Passes when every player connects, the shared
state (seed, tick, player records, both 25-park tables) is identical on every
machine just after the sync at the park gates, and a thug is in every park
but the sender's. SKIPs, like conformance.py, without game data or a build.

    py -3 tools/mpcheck.py [--players 4] [--exe build/themepark.exe]
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


def region(d, name, n):
    u32 = lambda a: struct.unpack_from('<I', d, a - B)[0]
    at, size = {'seed': (0x4C3AF4, 4), 'tick': (0x4C3AFC, 4), 'players': (0x4C443C, n * 0x6C2),
                'parks': (u32(0x4C3B00), 25 * 0x222), 'stock': (0x4BF1E0, 25 * 0x250)}[name]
    return d[at - B:at - B + size]


def thugs(d):
    u32 = lambda a: struct.unpack_from('<I', d, a - B)[0]
    return sum(d[a - B + 4] == 0x1A for a in range(u32(0x4C3B10) + 0xC8, u32(0x4C3B78), 0xC8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--players', type=int, default=4)
    ap.add_argument('--port', type=int, default=7790)
    ap.add_argument('--exe', default=os.path.join(HERE, 'build', 'themepark.exe') if os.name == 'nt'
                    else os.path.join(HERE, 'build-linux', 'themepark'))
    a = ap.parse_args()
    a.exe = os.path.abspath(a.exe)
    if not os.path.isdir(os.path.join(HERE, 'game', 'GAME')) or not os.path.exists(a.exe):
        print('SKIP: needs game/ and a build at %s' % a.exe)
        return 0
    n, tmp = a.players, tempfile.mkdtemp()
    procs = []
    for k in range(n):
        net = ['--host', str(n), '--port', str(a.port)] if k == 0 else ['--join', '127.0.0.1:%d' % a.port]
        cmd = [a.exe, '--headless', '--seconds', '70', '--fps', '30'] + net + SCRIPT + [
            '--dump', '45.5:' + os.path.join(tmp, 'gate%d.bin' % k),
            '--dump', '69:' + os.path.join(tmp, 'end%d.bin' % k)]
        if k == n - 1:
            cmd += ['--key', '60:1D', '--key', '60.05:14']   # Ctrl+T
        procs.append(subprocess.Popen(cmd, cwd=HERE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                      text=True, errors='replace'))
        time.sleep(0.5)
    errs = [p.communicate(timeout=900)[1] for p in procs]
    ok = True
    for k, e in enumerate(errs):
        joined = 'player %d of %d' % (k + 1, n) in e
        print('  player %d  %s' % (k + 1, 'connected' if joined else 'FAIL: did not connect'))
        ok &= joined
    if not ok:
        return 1
    gate = [open(os.path.join(tmp, 'gate%d.bin' % k), 'rb').read() for k in range(n)]
    for r in ('seed', 'tick', 'players', 'parks', 'stock'):
        same = all(region(g, r, n) == region(gate[0], r, n) for g in gate[1:])
        print('  sync %-8s %s' % (r, 'identical' if same else 'FAIL: differs'))
        ok &= same
    got = [thugs(open(os.path.join(tmp, 'end%d.bin' % k), 'rb').read()) for k in range(n)]
    want = [1] * (n - 1) + [0]
    print('  thugs per park %s (want %s)' % (got, want))
    ok &= got == want
    shutil.rmtree(tmp, ignore_errors=True)
    print('multiplayer %d players: %s' % (n, 'pass' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
