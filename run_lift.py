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
import re
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
    # The game moving the pointer is not the player: the attract demo kept ending.
    0x00401191: 'set_cursor',
    # Custom resolutions (#11): the 640x480 mode set and the VESA call under it.
    0x0046ECF0: 'hires_mode', 0x0046EC50: 'lores_mode', 0x0046E658: 'vesa_set',
    0x0044F63F: 'resource_load', 0x00444ED6: 'screen_update', 0x0043DFAC: 'park_zones',
    0x00445EFA: 'double_screen',   # the game's own screens, wider (#11)
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
    # The NetBIOS multiplayer, carried over TCP (net.c, docs/multiplayer.md).
    0x004393BE: 'net_start', 0x00438A01: 'net_present', 0x00439414: 'net_connect',
    0x00438F65: 'net_send', 0x004390DF: 'net_receive', 0x00439B20: 'net_hangup',
    0x004399F2: 'net_exchange', 0x0041CCF8: 'net_dispatch',
    # Modern rules, each off unless turned on (src/runtime/rules.c, docs/rules.md).
    0x00424149: 'guard_patrol', 0x0043D88D: 'thug_caught', 0x0041276C: 'guest_stuck', 0x0044CBDB: 'advisor',
    0x00422BCF: 'ent_perform', 0x00422A06: 'ent_walk', 0x004248ED: 'staff_drop',
    0x0042542D: 'strike_return', 0x004517E0: 'handyman_decide', 0x00453E5D: 'patrol_step',
    0x004187D8: 'fence_place', 0x00430DA6: 'lamp_place', 0x0041845A: 'guest_spawn', 0x004564F1: 'guest_spawn2',
    0x00430E92: 'fountain_place', 0x004105D7: 'bulldoze', 0x00437D38: 'palette_dim', 0x0040B837: 'park_view',
    0x00470990: 'song_stop', 0x00470FE0: 'effect_start', 0x00470C80: 'effect_note',
    0x004249EB: 'walk_step', 0x00423482: 'eng_walk', 0x00423E3F: 'eng_lunch',
    0x0046E888: 'key_read', 0x004460D2: 'world_map',
}

# INTRO.EXE, the opening film: the same libraries as MAIN.EXE, so the same
# overrides, found by matching MAIN.EXE's functions byte for byte (relocated
# words aside). It has no MIDI tempo call. Its sound bank variant byte and a
# lifted `ret` are in main.c.
INTRO_OVERRIDES = {
    0x00408340: 'hold_back_memory',
    0x00409CED: 'sos_ok', 0x00409EAA: 'sos_ok', 0x0040A498: 'sos_ok', 0x00409E56: 'sos_ok',
    0x004074FE: 'sos_ok', 0x0040755A: 'sos_ok', 0x00407CB6: 'sos_ok',
    0x0040A9C4: 'sos_ok', 0x0040A900: 'sos_ok',
    0x004068B2: 'sos_ok', 0x00406A4E: 'sos_ok', 0x00406F25: 'sos_ok', 0x00407F60: 'sos_ok',
    0x00409BE6: 'sos_ok',
    0x00407584: 'sos_digi_init_driver',
    0x0040AFDD: 'sos_digi_start', 0x0040AE79: 'sos_digi_done',
    0x0040B5E6: 'sos_digi_stop', 0x0040A938: 'sos_digi_volume',
    0x00406A78: 'sos_midi_init_driver', 0x00407036: 'sos_midi_reset',
    0x00401B4E: 'sos_midi_init_song', 0x004020C4: 'sos_midi_start', 0x0040214C: 'sos_midi_stop',
    0x00402081: 'sos_midi_uninit_song', 0x0040BFA7: 'sos_midi_done',
    0x0040256A: 'music_volume', 0x00409BB0: 'sos_midi_send',
}


def apply_override(body, addr, name):
    fn = ACTIVE.get(addr)
    if fn and body.startswith('void %s(void) {' % name):
        body = body.replace('void %s(void) {' % name, 'void %s_lifted(void) {' % name, 1)
        body = 'void %s(void (*lifted)(void));\n' % fn + body
        body += '\nvoid %s(void) { %s(%s_lifted); }\n' % (name, fn, name)
    return body


def prefix_symbols(out):
    """The intro links into the same exe as the game: its functions become
    intro_XXXXXXXX and its tables intro_dispatch_table and so on."""
    sym = re.compile(r'\bsub_([0-9A-F]{8})\b|\brecomp_(dispatch_table|dispatch_count|entry_va|image\w*)\b')
    for fn in os.listdir(out):
        if fn.endswith(('.c', '.h')):
            p = os.path.join(out, fn)
            src = open(p, encoding='utf-8').read()
            open(p, 'w', encoding='utf-8', newline='\n').write(
                sym.sub(lambda m: 'intro_' + (m.group(1) or m.group(2)), src))


# Custom resolutions (#11): instructions whose 640 or 480 is the park view's
# size rather than a coordinate, lifted with a variable (game.c) in its place.
# Each maps to (constant, variable); 640/480 unless a wider mode is chosen.
# generated by tools/screen_sites.py: 84 width, 111 height, 1 view-height, 78 row-multiple sites
SCREEN_SITES = {}
SCREEN_SITES.update({a: ('0x280u', 'tp_w') for a in (
    0x00401483, 0x00401621, 0x00401690, 0x004016A3, 0x004016B2, 0x004043BD, 0x00406CB8, 0x00406D70,
    0x00406E28, 0x00406EE0, 0x00407D41, 0x004084FB, 0x0040A1C0, 0x0040A2B4, 0x0040B9AA, 0x0040B9D1,
    0x0040BA07, 0x0040BF0F, 0x0040BF36, 0x0040BF6C, 0x0040C123, 0x0040C13C, 0x0040C154, 0x0040C824,
    0x0040FC8C, 0x0042C151, 0x0042C22A, 0x0042D317, 0x0042FF96, 0x00430047, 0x00430061, 0x0043014E,
    0x004304F4, 0x00430541, 0x00430582, 0x00435162, 0x004374F0, 0x00437702, 0x00437938, 0x00437AAB,
    0x00437B2E, 0x00437B3E, 0x00437E4C, 0x0043E2F6, 0x0044056A, 0x0044057C, 0x004405CB, 0x00440691,
    0x004406BD, 0x004406EE, 0x00445C3D, 0x00445C43, 0x00445DF9, 0x00445E69, 0x00445E6F, 0x00445EC0,
    0x00445EC6, 0x00449C19, 0x004539CC, 0x0045AC0E, 0x0045AC8D, 0x0045AE98, 0x0045BCE1, 0x0045C018,
    0x0045C01F, 0x00465CA1, 0x00465FB4, 0x0046C1AA, 0x0046C2C7, 0x0046C429, 0x0046D818, 0x0046D821,
    0x0046E197, 0x0046ED2A, 0x0046ED63, 0x00470C0C, 0x00470C4C, 0x00470C5F, 0x00470F43, 0x0047151E,
    0x00471551, 0x0047155D, 0x00471DA2, 0x00471DAE,
)})
SCREEN_SITES.update({a: ('0x1E0u', 'tp_h') for a in (
    0x0040147A, 0x0040163B, 0x00403BB8, 0x0040441F, 0x00405C9D, 0x00406CE8, 0x00406DA0, 0x00406E58,
    0x00406F10, 0x00407D69, 0x0040852B, 0x0040A1F0, 0x0040A2AB, 0x0040B88E, 0x0040B8BF, 0x0040C046,
    0x0040C105, 0x0040C7EA, 0x0042B442, 0x0042C181, 0x0042C25A, 0x0042D33C, 0x0042D375, 0x0042D411,
    0x0042D44A, 0x0042F99C, 0x0042FFC8, 0x00430087, 0x004300C0, 0x0043025E, 0x004302D2, 0x00430337,
    0x004303C0, 0x00436D91, 0x00436EB7, 0x0043754A, 0x0043764F, 0x00437955, 0x0043BD26, 0x0043DFD9,
    0x0043E008, 0x0043E034, 0x0043E056, 0x0043E07A, 0x0043E09E, 0x0043E0C0, 0x0043E0E5, 0x0043E10A,
    0x0043E12F, 0x0043E154, 0x0043E179, 0x0043E19E, 0x0043E1C3, 0x0043E1E8, 0x0043E20D, 0x0043E232,
    0x0043E257, 0x0043E27C, 0x0043E2A1, 0x0043E2C6, 0x0044040F, 0x00443D91, 0x0044533F, 0x004454E0,
    0x00445933, 0x00445B65, 0x00447C2F, 0x00449C49, 0x0044B8DE, 0x0044C2E7, 0x0044C346, 0x0044C9F3,
    0x0044E478, 0x0044E4CF, 0x0044E549, 0x0044E5A0, 0x0044E645, 0x0044E69B, 0x0044E6FD, 0x0044E753,
    0x0044E7C2, 0x0044E818, 0x0044E87A, 0x0044E8D0, 0x0044E92C, 0x0044E959, 0x00453A0D, 0x0045AC27,
    0x0045AEB1, 0x0045BCFA, 0x00466DF3, 0x0046888A, 0x00468913, 0x00468E62, 0x00468ED1, 0x00468FCA,
    0x00468FF1, 0x00469018, 0x0046902D, 0x004693F6, 0x0046941D, 0x00469444, 0x00469459, 0x0046B0FB,
    0x0046C1C6, 0x0046C247, 0x0046C2C0, 0x0046C41F, 0x0046ED34, 0x0046ED5D, 0x00470C58,
)})
SCREEN_SITES.update({a: ('0x190u', 'tp_vh') for a in (
    0x00445FAE,
)})
SCREEN_SITES.update({
    0x00400780: ('0x1DEu', 'tp_h - 2'),
    0x00400789: ('0x27Eu', 'tp_w - 2'),
    0x004011F8: ('0x27Eu', 'tp_w - 2'),
    0x00401200: ('0x27Eu', 'tp_w - 2'),
    0x00401294: ('0x1DEu', 'tp_h - 2'),
    0x0040129C: ('0x1DEu', 'tp_h - 2'),
    0x0040132A: ('0x27Eu', 'tp_w - 2'),
    0x00401332: ('0x27Eu', 'tp_w - 2'),
    0x004013F0: ('0x1DEu', 'tp_h - 2'),
    0x004013F8: ('0x1DEu', 'tp_h - 2'),
    0x00404163: ('0x1DEu', 'tp_h - 2'),
    0x0040416C: ('0x27Eu', 'tp_w - 2'),
    0x0043763B: ('0x27Eu', 'tp_w - 2'),
    0x00444EF5: ('0x1Eu', '(tp_h + 15) / 16'),
    0x00444F06: ('0x2800u', '16 * tp_w'),
    0x00445C54: ('0x500u', '2 * tp_w'),
    0x00445C5A: ('0x500u', '2 * tp_w'),
    0x00445C6B: ('0x780u', '3 * tp_w'),
    0x00445C71: ('0x780u', '3 * tp_w'),
    0x00445C82: ('0xA00u', '4 * tp_w'),
    0x00445C88: ('0xA00u', '4 * tp_w'),
    0x00445C99: ('0xC80u', '5 * tp_w'),
    0x00445C9F: ('0xC80u', '5 * tp_w'),
    0x00445CB0: ('0xF00u', '6 * tp_w'),
    0x00445CB6: ('0xF00u', '6 * tp_w'),
    0x00445CC7: ('0x1180u', '7 * tp_w'),
    0x00445CCD: ('0x1180u', '7 * tp_w'),
    0x00445CDE: ('0x1400u', '8 * tp_w'),
    0x00445CE4: ('0x1400u', '8 * tp_w'),
    0x00445CF5: ('0x1680u', '9 * tp_w'),
    0x00445CFB: ('0x1680u', '9 * tp_w'),
    0x00445D0C: ('0x1900u', '10 * tp_w'),
    0x00445D12: ('0x1900u', '10 * tp_w'),
    0x00445D23: ('0x1B80u', '11 * tp_w'),
    0x00445D29: ('0x1B80u', '11 * tp_w'),
    0x00445D3A: ('0x1E00u', '12 * tp_w'),
    0x00445D40: ('0x1E00u', '12 * tp_w'),
    0x00445D51: ('0x2080u', '13 * tp_w'),
    0x00445D57: ('0x2080u', '13 * tp_w'),
    0x00445D68: ('0x2300u', '14 * tp_w'),
    0x00445D6E: ('0x2300u', '14 * tp_w'),
    0x00445D7F: ('0x2580u', '15 * tp_w'),
    0x00445D85: ('0x2580u', '15 * tp_w'),
    0x00445FB6: ('0x3200u', 'tp_w * 20'),
    0x0044B949: ('0x1DEu', 'tp_h - 2'),
    0x0044B952: ('0x27Eu', 'tp_w - 2'),
    0x0044BCE0: ('0x1DEu', 'tp_h - 2'),
    0x0044BCE9: ('0x27Eu', 'tp_w - 2'),
    0x0044C1BB: ('0x1DEu', 'tp_h - 2'),
    0x0044C1C4: ('0x27Eu', 'tp_w - 2'),
    0x0044C682: ('0x1DEu', 'tp_h - 2'),
    0x0044C68B: ('0x27Eu', 'tp_w - 2'),
    0x00464271: ('0x1DEu', 'tp_h - 2'),
    0x0046427A: ('0x27Eu', 'tp_w - 2'),
    0x0046D95F: ('0x27Eu', 'tp_w - 2'),
    0x0046D96A: ('0x27Eu', 'tp_w - 2'),
    0x0046D988: ('0x1DEu', 'tp_h - 2'),
    0x0046D993: ('0x1DEu', 'tp_h - 2'),
    0x0046DA7B: ('0x27Eu', 'tp_w - 2'),
    0x0046DA86: ('0x27Eu', 'tp_w - 2'),
    0x0046DAA4: ('0x1DEu', 'tp_h - 2'),
    0x0046DAAF: ('0x1DEu', 'tp_h - 2'),
    0x0046DB16: ('0x27Eu', 'tp_w - 2'),
    0x0046DB21: ('0x27Eu', 'tp_w - 2'),
    0x0046DB3F: ('0x1DEu', 'tp_h - 2'),
    0x0046DB4A: ('0x1DEu', 'tp_h - 2'),
    0x0046DB93: ('0x27Eu', 'tp_w - 2'),
    0x0046DB9E: ('0x27Eu', 'tp_w - 2'),
    0x0046DBBC: ('0x1DEu', 'tp_h - 2'),
    0x0046DBC7: ('0x1DEu', 'tp_h - 2'),
    0x0046DC27: ('0x27Eu', 'tp_w - 2'),
    0x0046DC32: ('0x27Eu', 'tp_w - 2'),
    0x0046DC3B: ('0x1DEu', 'tp_h - 2'),
    0x0046DC46: ('0x1DEu', 'tp_h - 2'),
    0x00470F23: ('0xFFFFF884u', '4 - 3 * tp_w'),
    0x00470FC9: ('0xFFFFF104u', '4 - 6 * tp_w'),
    0x004715C5: ('0xA0u', 'tp_w / 4'),
    0x00471CC2: ('0xA0u', 'tp_w / 4'),
    # The bank switch turns colour 0 red past bank 5, a debug check; a wide
    # screen has more banks (#11: the red tinge). 1920x480 ends in bank 14.
    0x0046E6D1: ('(5)', '(tp_w == 640 ? 5 : 15)'),
})
# The camera 3 rows past the back of the park, with the building rule
# (RULE_BUILD = 32, rules.c view_above; #15): the edge scroll's and the keys'
# clamps, and the pointer above the map picks row 0.
CAM_TOP = '(tp_rules & 32u ? -3 : 0)'
SCREEN_SITES.update({
    0x004043EE: ('(uint32_t)(0)', CAM_TOP),
    0x00408583: ('(uint32_t)(0)', CAM_TOP),
    0x0040858D: ('(uint16_t)(0)', CAM_TOP),
    0x0043751C: ('(uint32_t)(0)', CAM_TOP),   # the edge scroll while laying track (0x437268)
    0x00404B90: ('(uint16_t)(0)', '((int16_t)MEM16(0x4C6666) < 0 ? -(int16_t)MEM16(0x4C6666) : 0)'),
    # The cursor keys' clamp stops where the edge scroll does (rules.c tp_cam_limit).
    0x0040855B: ('(uint32_t)(0)', "tp_cam_limit('l')"),
    0x00408565: ('(uint16_t)(0)', "tp_cam_limit('l')"),
    0x00408570: ('(uint32_t)(0x40u)', "tp_cam_limit('r') + 1"),
    0x0040857A: ('(uint16_t)(0x3Fu)', "tp_cam_limit('r')"),
    0x00408598: ('(uint32_t)(0x40u)', "tp_cam_limit('b') + 1"),
    0x004085A2: ('(uint16_t)(0x3Fu)', "tp_cam_limit('b')"),
})
# Negotiation (#49): with the deal rule (RULE_DEAL = 256) the player's hand
# (offer 0x4C77FC, from the floor 0x4C77F8 up to 0x4C7800) moves 4 a frame,
# not 2, without passing either end. The countdown is untouched.
SCREEN_SITES.update({
    0x00435546: ('MEM32(0x4C77FC) - 2', 'MEM32(0x4C77FC) - (tp_rules & 256u && (int32_t)MEM32(0x4C77FC) - 4 >= (int32_t)MEM32(0x4C77F8) ? 4u : 2u)'),
    0x00435658: ('MEM32(0x4C77FC) + 2', 'MEM32(0x4C77FC) + (tp_rules & 256u && (int32_t)MEM32(0x4C77FC) + 4 <= (int32_t)MEM32(0x4C7800) ? 4u : 2u)'),
})
SCREEN_SITES.update({
    0x004023C0: ('ecx = 0x2C00u;', '@tp_present_tail'),
    0x0040AF68: ('ecx = 0x2C00u;', '@tp_present_tail'),
    0x0040B7AF: ('ecx = 0x2C00u;', '@tp_present_tail'),
    0x004298BE: ('ecx = 0x2C00u;', '@tp_present_tail'),
    0x00436CF2: ('ecx = 0x2C00u;', '@tp_present_tail'),
    0x00436E5A: ('ecx = 0x2C00u;', '@tp_present_tail'),
    0x00445B57: ('ecx = 0x2C00u;', '@tp_present_tail'),
    0x00471D5D: ('ecx = 0x2C00u;', '@tp_present_tail'),
    0x00446061: ('ecx = 0x2C00u;', '@tp_clear_tail'),
})


def patch_screen_sites(out):
    """Put the view-size variables in at SCREEN_SITES (and on a cmp's flag line)."""
    done = set()
    for fn in os.listdir(out):
        if not (fn.startswith('recomp_0') and fn.endswith('.c')):
            continue
        p = os.path.join(out, fn)
        lines = open(p, encoding='utf-8').read().split('\n')
        for i, l in enumerate(lines):
            m = re.search(r'/\* 0x([0-9A-F]{8}): ', l)
            site = m and SCREEN_SITES.get(int(m.group(1), 16))
            if not site:
                continue
            const, var = site
            if var.startswith('@'):      # the statement becomes a host call
                lines[i] = lines[i].replace(const, 'RECOMP_REGS_OUT(); %s(); RECOMP_REGS_IN();' % var[1:])
            else:
                for j in (i, i + 1):
                    if j == i or lines[j].lstrip().startswith('_flag_a'):
                        lines[j] = lines[j].replace(const, '(uint32_t)(' + var + ')')
            done.add(int(m.group(1), 16))
        lines.insert(lines.index('#include "recomp_funcs.h"') + 1,
                     'extern int32_t tp_w, tp_h, tp_vh;\nextern uint32_t tp_rules;\nint32_t tp_cam_limit(int edge);\nvoid tp_present_tail(void);\nvoid tp_clear_tail(void);')
        open(p, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    missing = set(SCREEN_SITES) - done
    if missing:
        print('[!] screen sites not found: ' + ' '.join('0x%08X' % a for a in sorted(missing)))


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


INTRO_SHA1 = 'fdb4076efa432ae572be1968a663d2355f9b85c6'   # GOG's and the 1994 CD's: the same file


def intro_exe(path):
    data = open(path, 'rb').read()
    sha = hashlib.sha1(data).hexdigest()
    if sha != INTRO_SHA1:
        sys.exit('run_lift: %s is an INTRO.EXE this lift does not know (SHA-1 %s).\n'
                 'Please open an issue with that SHA-1.' % (path, sha))
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--intro', action='store_true', help='lift INTRO.EXE (the opening film) instead')
    ap.add_argument('--exe')
    ap.add_argument('--catalog')
    ap.add_argument('--out')
    ap.add_argument('--split', type=int, default=400)
    args = ap.parse_args()
    global ACTIVE
    if args.intro:
        ACTIVE, name, cat_name, out = INTRO_OVERRIDES, 'INTRO.EXE', 'intro_funcs.json', 'gen_intro'
    else:
        ACTIVE, name, cat_name, out = OVERRIDES, 'MAIN.EXE', 'd32_funcs.json', 'gen'
    args.exe = args.exe or os.path.join(_HERE, 'game', 'GAME', name)
    args.catalog = args.catalog or os.path.join(_HERE, 'analysis', cat_name)
    args.out = args.out or os.path.join(_HERE, 'src', 'recomp', out)

    le = LEImage(intro_exe(args.exe) if args.intro else main_exe(args.exe))
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
        stem = os.path.splitext(cat_name)[0]
        pe_path, seed_path = os.path.join(work, stem + '_pe.exe'), os.path.join(work, stem + '_seeds.json')
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

    if args.intro:
        prefix_symbols(args.out)
    else:
        patch_screen_sites(args.out)

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out) if fn.startswith('recomp_'))
    stats = {'functions': len(entries), 'errors': errors, 'files': idx, 'lines': lines}
    json.dump(stats, open(os.path.join(_HERE, 'analysis', 'lift_stats_intro.json' if args.intro
                                       else 'lift_stats.json'), 'w'), indent=1)
    print('=' * 60)
    print('  functions %d   errors %d   files %d   lines %s   %.1fs'
          % (len(entries), errors, idx, format(lines, ','), time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
