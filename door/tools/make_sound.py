#!/usr/bin/env python3
"""
make_sound.py -- turns wolfrender's WAV files into what the JPEG XL graphics mode plays (door/sound/).

    ./wolfrender ../data /tmp/wolfsound && python3 tools/make_sound.py /tmp/wolfsound sound

  digi_NN.wav        the digitised sounds, copied as they are (8-bit mono at 7042 Hz: a few KB each)
  adlib_NN.ogg       the AdLib sound effects, Ogg Vorbis, mono at 22050 Hz, at quality 8: at quality 0 (the DOOM
                     door's) these short FM tones came out gritty (15-19 dB signal to noise; 27-35 at 8), and a Vorbis
                     file's headers are most of an effect's size anyway (4.3 KB -> 6 KB for the 'nothing to use' sound)
  music_NN_kk.ogg    each track cut into 5-second Ogg Vorbis pieces, mono at 22050 Hz, quality 4 (about 5.5 KB a
                     second; quality 0 was 17-21 dB signal to noise and audibly gritty, 4 is 23-29). The
                     door uploads a piece only when it's about to be played, and the caller's terminal plays the
                     pieces back to back on one channel, seamlessly (Vorbis keeps each piece's exact length).
  index.txt          one line per sound: "digi N", "adlib N", or "music N <pieces> <length of the last piece in ms>"

Derived from id Software's data, like the data itself: shipped with the door, not in the public source.
Needs ffmpeg with libvorbis. (The DOOM door's tools/make_music.py, for Wolfenstein.)
"""

import os
import shutil
import subprocess
import sys

RATE = 22050
PIECE = 5 * RATE          # samples a piece
MUSIC_QUALITY = '4'
EFFECT_QUALITY = '8'


def to_pcm(path):
    """The whole file, mono at 22050 Hz, as raw samples: pieces are cut from this so they join exactly"""
    return subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', path, '-ac', '1', '-ar', str(RATE),
                           '-f', 's16le', '-'], check=True, capture_output=True).stdout


def to_ogg(pcm, path, quality):
    # bitexact: the same stream serial every time, so re-running this doesn't change a file that hasn't changed (its
    # md5 is how the door knows a caller already has it; a new one sends it to every caller again)
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-f', 's16le', '-ar', str(RATE), '-ac', '1', '-i', '-',
                    '-c:a', 'libvorbis', '-q:a', quality, '-fflags', '+bitexact', '-flags:a', '+bitexact', path],
                   input=pcm, check=True)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(dst):
        if name.endswith('.ogg') or name.endswith('.wav') or name == 'index.txt':
            os.remove(os.path.join(dst, name))

    index = []
    for line in open(os.path.join(src, 'sounds.txt')):
        parts = line.split()
        if not parts:
            continue
        kind, n = parts[0], int(parts[1])
        if kind == 'digi':
            shutil.copy(os.path.join(src, f'digi_{n:02d}.wav'), os.path.join(dst, f'digi_{n:02d}.wav'))
            index.append(f'digi {n}\n')
        elif kind == 'adlib':
            to_ogg(to_pcm(os.path.join(src, f'adlib_{n:02d}.wav')), os.path.join(dst, f'adlib_{n:02d}.ogg'), EFFECT_QUALITY)
            index.append(f'adlib {n}\n')
        elif kind == 'music':
            pcm = to_pcm(os.path.join(src, f'music_{n:02d}.wav'))
            samples = len(pcm) // 2
            pieces = (samples + PIECE - 1) // PIECE
            for k in range(pieces):
                to_ogg(pcm[k * PIECE * 2:(k + 1) * PIECE * 2], os.path.join(dst, f'music_{n:02d}_{k:02d}.ogg'), MUSIC_QUALITY)
            last_ms = (samples - (pieces - 1) * PIECE) * 1000 // RATE
            index.append(f'music {n} {pieces} {last_ms}\n')
            print(f'music {n}: {pieces} pieces')
    with open(os.path.join(dst, 'index.txt'), 'w') as f:
        f.writelines(index)


if __name__ == '__main__':
    main()
