# psxmusic.py DISC.cue [--name psx] [--wav]
# Make a music pack from your own PlayStation Theme Park disc (a bin/cue rip):
# the CD tracks RIDE1..RIDE17 are recordings of the DOS game's ride tunes, and
# HEART is the negotiation heartbeat (HRTBEAT). Writes music\NAME\SONG.ogg
# (or .wav with --wav, or when ffmpeg is not on the PATH).
# The disc's CDA\ folder names every track, so any pressing works.
import argparse, os, re, shutil, struct, subprocess, sys, wave

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SONGS = {'HEART': 'HRTBEAT', **{'RIDE%d' % i: 'RIDE%d' % i for i in range(1, 18)}}
RAW = 2352


def msf(s):
    m, sec, fr = map(int, s.split(':'))
    return (m * 60 + sec) * 75 + fr


def read_cue(path):
    """Tracks as dicts: num, mode, file, i0/i1 (sectors from the file's start), base (disc LBA of the file)."""
    tracks, cur, base = [], None, 0
    folder = os.path.dirname(os.path.abspath(path))
    for line in open(path, encoding='latin-1'):
        w = line.split()
        if not w:
            continue
        if w[0] == 'FILE':
            if cur:
                base += os.path.getsize(cur) // RAW
            cur = os.path.join(folder, re.match(r'\s*FILE\s+"?(.*?)"?\s+\w+\s*$', line).group(1))
        elif w[0] == 'TRACK':
            tracks.append({'num': int(w[1]), 'mode': w[2], 'file': cur, 'base': base})
        elif w[0] == 'INDEX':
            tracks[-1]['i%d' % int(w[1])] = msf(w[2])
    return tracks


def cda_names(t1):
    """{disc LBA: name} from the data track's CDA\\ folder (ISO 9660, MODE2/2352)."""
    f = open(t1['file'], 'rb')

    def block(lba):
        f.seek((lba - t1['base']) * RAW + 24)
        return f.read(2048)

    def entries(rec):
        lba, size = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        for b in range((size + 2047) // 2048):
            d, o = block(lba + b), 0
            while o < 2048 and d[o]:
                r = d[o:o + d[o]]
                yield r[33:33 + r[32]].decode('latin-1').split(';')[0], r
                o += d[o]

    pvd = block(t1['base'] + 16)
    if pvd[1:6] != b'CD001':
        return {}
    for name, rec in entries(pvd[156:190]):
        if name.upper() == 'CDA':
            return {struct.unpack_from('<I', r, 2)[0]: n.split('.')[0].upper()
                    for n, r in entries(rec) if n not in ('\0', '\1')}
    return {}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('cue')
    ap.add_argument('--name', default='psx', help='the pack folder under music\\ (default psx)')
    ap.add_argument('--wav', action='store_true', help='write WAV, not Ogg')
    a = ap.parse_args()

    tracks = read_cue(a.cue)
    names = cda_names(tracks[0])
    if not names:
        sys.exit('no CDA folder on the data track: is this the PlayStation Theme Park disc?')
    ffmpeg = None if a.wav else shutil.which('ffmpeg')
    if not a.wav and not ffmpeg:
        print('ffmpeg not found: writing WAV (about 140 MB; Ogg is about 10 MB)')
    out = os.path.join(HERE, 'music', a.name)
    os.makedirs(out, exist_ok=True)

    done = 0
    for i, t in enumerate(tracks):
        if t['mode'] != 'AUDIO':
            continue
        start = t['base'] + t['i1']
        # The CDA entry points just past the track's start.
        name = next((n for lba, n in names.items() if 0 <= lba - start <= 4), None)
        song = SONGS.get(name)
        if not song:
            print('track %2d  %-8s skipped (the DOS game has no song for it)' % (t['num'], name or '?'))
            continue
        nxt = tracks[i + 1] if i + 1 < len(tracks) else None
        if nxt and nxt['file'] == t['file']:
            end = nxt.get('i0', nxt['i1'])
        else:
            end = os.path.getsize(t['file']) // RAW
        with open(t['file'], 'rb') as f:
            f.seek(t['i1'] * RAW)
            pcm = f.read((end - t['i1']) * RAW)
        dest = os.path.join(out, song + '.wav')
        with wave.open(dest, 'wb') as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
            w.writeframes(pcm)
        if ffmpeg:
            ogg = dest[:-4] + '.ogg'
            subprocess.run([ffmpeg, '-loglevel', 'error', '-y', '-i', dest, '-c:a', 'libvorbis', '-q:a', '6', ogg], check=True)
            os.remove(dest)
            dest = ogg
        print('track %2d  %-8s -> %s  %.1f s' % (t['num'], name, os.path.relpath(dest, HERE), len(pcm) / 176400))
        done += 1
    print('%d songs in music\\%s: pick "%s" in F10, Music' % (done, a.name, a.name))


if __name__ == '__main__':
    main()
