#!/usr/bin/env python3
"""Theme Park (DOS, Bullfrog 1994, GOG 1.01) lift driver.

MAIN.EXE is a Watcom C/C++32 program bound to DOS/4GW: an MZ stub and an LE
image. pcrecomp's LE front end (tools/le/le_parse.py) lays the objects out at
LOAD_BASE with every fixup applied and re-wraps them as a PE32, so the rest is
the ordinary 32-bit pipeline: disasm32 finds the functions, lift32 lifts them
with dos=True (int, port I/O, cli/sti and segment loads call into the DOS
host in src/runtime/), and generate.py writes the chunks.

Function entries: disasm32's catalog, seeded with every fixup that points
into the code object (call tables, Watcom's in-code switch tables, ISRs).

Output goes to src/recomp/gen/, which is gitignored: it is derived from the
game binary and never distributed (see README). recomp_image.c there carries
the relocated image the host copies into guest memory at start-up.

    py -3 run_lift.py [--exe game/GAME/MAIN.EXE] [--out src/recomp/gen]
"""
import argparse
import bisect
import hashlib
import json
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# pcrecomp checkout: $PCRECOMP, else a sibling ../tools
_PC = os.path.join(os.environ.get('PCRECOMP') or os.path.join(_HERE, '..', 'tools'), 'tools')
sys.path.insert(0, os.path.join(_PC, 'lift'))
sys.path.insert(0, os.path.join(_PC, 'le'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32            # noqa: E402
from generate import (LinearInstruction,                     # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from le_parse import LEImage, code_pointers, to_pe          # noqa: E402
from lift32 import Lifter                                   # noqa: E402
from recover import recover_functions                       # noqa: E402

# Where the LE objects go in guest memory. DOS/4GW would put them anywhere
# above 1 MB; the linker's own bases (0x10000, 0xA0000, ...) collide with
# conventional memory and the VGA window, so move the lot to 4 MB.
LOAD_BASE = 0x400000

# Functions whose body is hand-written in src/runtime/ instead of lifted.
HOST_SHIM = {
}

# Host functions that take over a lifted one (src/runtime/game.c). Each gets
# the lifted body as `void (*lifted)(void)` and calls it when it wants to.
OVERRIDES = {
    # The input poll: every loop the game has (title, name prompt, park,
    # menus) calls it once a frame, and it counts idle frames toward the
    # 3000-frame attract-mode timeout. The game ran flat out on a 486 and its
    # pace was however long a frame took; native, every one of those loops
    # spins thousands of times a second. Pacing here restores the speed.
    0x0040107F: 'pace_frame',
    # The presents: loops that never poll (the year-end report, the
    # negotiations) are paced where they show a frame (game.c).
    0x00470210: 'pace_present', 0x00445507: 'pace_present',
    # The wait for retrace: the negotiations' own loop is paced here.
    0x00470BE4: 'pace_retrace',
    # Bullfrog's memory manager grabs all memory: leave the C library some.
    0x00472950: 'hold_back_memory',
    # The pointer's buffers, taken once and not again at every resolution change (#6).
    0x0046DF44: 'mouse_setup', 0x0046E8D0: 'pool_alloc',
    # HMI SOS, replaced at its edge (src/runtime/sound.c, docs/sound.md).
    # Detection, system init and shutdown: there is no card to find.
    0x00475793: 'sos_ok', 0x00475950: 'sos_ok', 0x00475F3E: 'sos_ok', 0x004758FC: 'sos_ok',
    0x00474C12: 'sos_ok', 0x00474C6E: 'sos_ok', 0x004753CA: 'sos_ok',
    0x00477A84: 'sos_ok', 0x004779C0: 'sos_ok',
    0x00473FC6: 'sos_ok', 0x00474162: 'sos_ok', 0x00474639: 'sos_ok', 0x0047564A: 'sos_ok',
    0x00477238: 'sos_ok',
    # Digital: driver init, start, done, stop, volume.
    0x00474C98: 'sos_digi_init_driver',
    0x0047943F: 'sos_digi_start', 0x004792DB: 'sos_digi_done',
    0x00479A48: 'sos_digi_stop', 0x004779F8: 'sos_digi_volume',
    # MIDI: driver init and reset, songs, tempo; and the game's music volume.
    0x0047418C: 'sos_midi_init_driver', 0x0047474A: 'sos_midi_reset',
    0x0046F68F: 'sos_midi_init_song', 0x0046FC05: 'sos_midi_start', 0x0046FC8D: 'sos_midi_stop',
    0x0046FBC2: 'sos_midi_uninit_song', 0x00472174: 'sos_midi_done', 0x00479E5C: 'sos_midi_tempo',
    0x004700AB: 'music_volume', 0x00477202: 'sos_midi_send',
}


def apply_override(body, addr, name):
    fn = OVERRIDES.get(addr)
    if fn and body.startswith('void %s(void) {' % name):
        body = body.replace('void %s(void) {' % name, 'void %s_lifted(void) {' % name, 1)
        body = 'void %s(void (*lifted)(void));\n' % fn + body
        body += '\nvoid %s(void) { %s(%s_lifted); }\n' % (name, fn, name)
    return body


def head_disassemble_function(md, code, cs, start, end, heads):
    """Decode only at disasm32's instruction heads in [start, end).

    Watcom keeps switch tables in the code object next to the jump, and a
    linear sweep decodes a table as instructions. Recursive descent already
    separated code from data, so ask it.
    """
    lo, hi = bisect.bisect_left(heads, start), bisect.bisect_left(heads, end)
    insns, leaders = [], {start}
    for h in heads[lo:hi]:
        i = next(md.disasm(code[h - cs:h - cs + 16], h, count=1), None)
        if i is None:
            continue
        li = LinearInstruction(i)
        if insns and insns[-1].end_address != h:
            leaders.add(h)               # after a table: reached only by a jump
        insns.append(li)
        if li.is_cond_jump or li.is_uncond_jump:
            t = li.get_branch_target()
            if t and start <= t < end:
                leaders.add(t)
            leaders.add(li.end_address)
    return insns, leaders


def write_image(path, img, base, entry, stack):
    """The relocated image as a C array, for the host to copy into guest memory."""
    with open(path, 'w', newline='\n') as f:
        f.write('/* Theme Park recomp - AUTO-GENERATED from MAIN.EXE: never distribute */\n'
                '#include <stdint.h>\n'
                'const uint32_t recomp_image_base = 0x%08Xu;\n'
                'const uint32_t recomp_image_size = %du;\n'
                'const uint32_t recomp_image_stack = 0x%08Xu;\n'
                'const uint8_t recomp_image[] = {\n' % (base, len(img), stack))
        for i in range(0, len(img), 32):
            f.write(','.join(str(b) for b in img[i:i + 32]) + ',\n')
        f.write('};\n')


# The MAIN.EXEs this lift knows, by SHA-1. The 1994 CD's (UK release and EA's
# CD-ROM Classics reissue alike) is GOG's byte for byte but for two fixes GOG
# made to the mouse code: the handler's early exit at 0x46D87E also clears the
# flag at 0x4AEC98, and the mouse set-up at 0x46DFFF stops allocating its three
# 4 KB buffers again every time it runs. Applying them makes the two identical,
# so every address in src/runtime/ holds for both.
GOG_SHA1 = '79678febe980b2fafd0aea96e83d4a6633449410'
CD1994_SHA1 = '3d8354ef2c6f1cb2bd0d66163ea1cbe103b1c373'
CD1994_TO_GOG = [  # (file offset, 1994 bytes, GOG bytes)
    (0x99680, '4d', '43'),
    (0x99DFF, '833d90e50200007409833d64e50200007502eb09833d54e50200007505e9b1000000',
              'eb0490909090e804000000909090905883e841eb0590909090902ec700e953000090'),
]


def main_exe(path):
    data = bytearray(open(path, 'rb').read())
    sha = hashlib.sha1(data).hexdigest()
    if sha == CD1994_SHA1:
        print('[*] MAIN.EXE from the 1994 CD: applying GOG\'s two mouse fixes')
        for off, old, new in CD1994_TO_GOG:
            assert data[off:off + len(old) // 2] == bytes.fromhex(old)
            data[off:off + len(new) // 2] = bytes.fromhex(new)
        sha = hashlib.sha1(data).hexdigest()
    if sha != GOG_SHA1:
        sys.exit('run_lift: %s is a MAIN.EXE this lift does not know (SHA-1 %s).\n'
                 'Known: GOG 1.01 and the 1994 CD. Please open an issue with that SHA-1.' % (path, sha))
    return bytes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(_HERE, 'game', 'GAME', 'MAIN.EXE'))
    ap.add_argument('--catalog', default=os.path.join(_HERE, 'analysis', 'd32_funcs.json'))
    ap.add_argument('--out', default=os.path.join(_HERE, 'src', 'recomp', 'gen'))
    ap.add_argument('--split', type=int, default=400)
    args = ap.parse_args()

    le = LEImage(main_exe(args.exe))
    img, lo, objs, relocs = le.build(LOAD_BASE)
    entry, stack = le.entry(objs), le.stack(objs)
    code_obj = next(o for o in objs if o['flags'] & 0x4)
    cs, ce = code_obj['addr'], code_obj['addr'] + code_obj['vsize']
    code = bytes(img[cs - lo:ce - lo])
    seeds = code_pointers(objs, relocs, img, lo)
    print('[*] LE: %d objects, %d fixups, code 0x%08X-0x%08X, entry 0x%08X, %d code pointers'
          % (len(objs), len(relocs), cs, ce, entry, len(seeds)))

    os.makedirs(os.path.dirname(args.catalog), exist_ok=True)
    if not os.path.exists(args.catalog):
        print('[*] no catalog yet: running disasm32 (a few minutes)')
        work = os.path.join(_HERE, 'work')
        os.makedirs(work, exist_ok=True)
        pe_path, seed_path = os.path.join(work, 'main_pe.exe'), os.path.join(work, 'seeds.json')
        open(pe_path, 'wb').write(to_pe(img, lo, objs, relocs, entry))
        json.dump(['0x%x' % s for s in seeds], open(seed_path, 'w'))
        subprocess.run([sys.executable, os.path.join(_PC, 'disasm', 'disasm32.py'), pe_path,
                        '--full', '--seed-functions', seed_path, '--output', args.catalog], check=True)
    cat = json.load(open(args.catalog, encoding='utf-8'))
    fns = sorted((f['address'], min(f['end'], ce)) for f in cat['functions'] if cs <= f['address'] < ce)
    heads = sorted({i['address'] for f in cat['functions'] for b in f['blocks'] for i in b['instructions']})
    del cat
    print('[*] disasm32: %d functions, %d instruction heads' % (len(fns), len(heads)))
    # Tail-jump targets and fixup pointers the catalog did not list as entries
    # (the entry point itself is `jmp` over the Watcom banner). An entry that
    # lands inside a known instruction would decode garbage over real code.
    starts = {a for a, _ in fns}
    extra = recover_functions(code, cs, ce, fns, forced=sorted(s for s in seeds if s not in starts))
    md1 = Cs(CS_ARCH_X86, CS_MODE_32)
    interior = set()
    for h in heads:
        i = next(md1.disasm(code[h - cs:h - cs + 16], h, count=1), None)
        if i is not None:
            interior.update(range(h + 1, h + i.size))
    extra = [e for e in extra if e[0] not in interior]
    print('[*] recovered %d more entries' % len(extra))
    fns = sorted(set(fns) | set(extra))
    known = {a for a, _ in fns}
    lost = [s for s in seeds if s not in known]
    if lost:
        print('[!] %d fixup code pointers are not function entries: %s'
              % (len(lost), ', '.join('0x%08X' % t for t in lost[:8])))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    # Call targets that start by popping their return address (see lift32).
    call_pop = {a for a in known if 0x58 <= code[a - cs] <= 0x5F}
    lifter = Lifter(lifted=known, dos=True, call_pop=call_pop)
    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):               # a smaller lift leaves stale chunks behind
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))
    entries, chunk, idx, errors, t0 = [], [], 0, 0, time.time()
    seen = set()
    for addr, end in fns:
        if addr in seen:
            continue
        seen.add(addr)
        name = 'sub_%08X' % addr
        if addr in HOST_SHIM:
            chunk.append(('/* %s: host shim */\n' % name, addr, name))
        else:
            try:
                insns, leaders = head_disassemble_function(md, code, cs, addr, end, heads)
                if not insns:
                    insns, leaders = linear_disassemble_function(md, code, cs, addr, end)
                body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                        else 'void %s(void) { }\n' % name)
                chunk.append((apply_override(body, addr, name), addr, name))
            except Exception as e:                              # noqa: BLE001
                chunk.append(('/* ERROR %s: %s */\nvoid %s(void) {}\n' % (name, e, name), addr, name))
                errors += 1
        entries.append((addr, name))
        if len(chunk) >= args.split:
            write_chunk(args.out, idx, chunk)
            idx += 1
            chunk = []
            print('[*]   %d/%d (%d err)' % (len(entries), len(fns), errors), flush=True)
    if chunk:
        write_chunk(args.out, idx, chunk)
        idx += 1

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* Theme Park recomp - AUTO-GENERATED */\n#pragma once\n#include <stdint.h>\n\n')
        for a, n in entries:
            f.write('void %s(void);  /* 0x%08X */\n' % (n, a))
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* Theme Park recomp - AUTO-GENERATED */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n' % len(entries))
        f.write('const uint32_t recomp_entry_va = 0x%08Xu;\n' % entry)
    write_image(os.path.join(args.out, 'image_data.c'), img, lo, entry, stack)

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out) if fn.startswith('recomp_'))
    stats = {'functions': len(entries), 'errors': errors, 'files': idx, 'lines': lines}
    json.dump(stats, open(os.path.join(_HERE, 'analysis', 'lift_stats.json'), 'w'), indent=1)
    print('=' * 60)
    print('  functions %d   errors %d   files %d   lines %s   %.1fs'
          % (len(entries), errors, idx, format(lines, ','), time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
