#!/usr/bin/env python3
"""
Plays the door's ANSI mode headless: runs wolf3ddoor on a pseudo-terminal as a caller without TRACE would see it,
types a script of keys, and renders the screen to PNGs so it can be looked at without a BBS or a terminal.

    python3 test_ansi.py [mode] [outdir]      mode: 2 = 24-bit (default), 3 = 256, 4 = 16 colours

The renderer understands only what the door sends: cursor moves, SGR colours (16, 256, 24-bit), CP437 blocks and
shades, and plain text. Keys are timed from the moment the game starts.
"""

import os
import pty
import re
import select
import subprocess
import sys
import time

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
DOOR = os.path.join(HERE, 'wolf3ddoor')
COLS, ROWS = 80, 24
CW, CH = 8, 16

VGA = [(0, 0, 0), (170, 0, 0), (0, 170, 0), (170, 85, 0), (0, 0, 170), (170, 0, 170), (0, 170, 170), (170, 170, 170),
       (85, 85, 85), (255, 85, 85), (85, 255, 85), (255, 255, 85), (85, 85, 255), (255, 85, 255), (85, 255, 255),
       (255, 255, 255)]


def xterm256(n):
    if n < 16:
        return VGA[[0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15][n]]   # ANSI order to VGA order
    if n < 232:
        n -= 16
        lv = [0, 95, 135, 175, 215, 255]
        return (lv[n // 36], lv[(n // 6) % 6], lv[n % 6])
    g = 8 + (n - 232) * 10
    return (g, g, g)


class Screen:
    ANSI_TO_VGA = [0, 4, 2, 6, 1, 5, 3, 7]

    def __init__(self):
        self.cells = [[(' ', (170, 170, 170), (0, 0, 0))] * COLS for _ in range(ROWS)]
        self.row = self.col = 0
        self.fg, self.bg, self.bold = (170, 170, 170), (0, 0, 0), False
        self.fg_index = 7
        self.pending = b''

    def sgr(self, params):
        nums = [int(p) if p else 0 for p in params.split(';')] if params else [0]
        i = 0
        while i < len(nums):
            n = nums[i]
            if n == 0:
                self.fg, self.bg, self.bold, self.fg_index = (170, 170, 170), (0, 0, 0), False, 7
            elif n == 1:
                self.bold = True
                if self.fg_index is not None and self.fg_index < 8:
                    self.fg = VGA[self.ANSI_TO_VGA[self.fg_index] + 8]
            elif 30 <= n <= 37:
                self.fg_index = n - 30
                self.fg = VGA[self.ANSI_TO_VGA[n - 30] + (8 if self.bold else 0)]
            elif 40 <= n <= 47:
                self.bg = VGA[self.ANSI_TO_VGA[n - 40]]
            elif n in (38, 48) and i + 1 < len(nums):
                if nums[i + 1] == 2 and i + 4 < len(nums):
                    c = tuple(nums[i + 2:i + 5])
                    i += 4
                elif nums[i + 1] == 5 and i + 2 < len(nums):
                    c = xterm256(nums[i + 2])
                    i += 2
                else:
                    c = (0, 0, 0)
                if n == 38:
                    self.fg, self.fg_index = c, None
                else:
                    self.bg = c
            i += 1

    def feed(self, data):
        data = self.pending + data
        self.pending = b''
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0x1B:
                if i + 1 >= len(data):
                    self.pending = data[i:]
                    return
                if data[i + 1] == ord('['):
                    m = re.match(rb'\x1b\[([?=]?)([0-9;]*)([@-~])', data[i:])
                    if not m:
                        self.pending = data[i:]
                        return
                    priv, params, final = m.group(1), m.group(2).decode(), chr(m.group(3)[0])
                    if not priv:
                        if final == 'H':
                            p = [int(x) if x else 1 for x in params.split(';')] if params else [1, 1]
                            self.row, self.col = (p[0] - 1, (p[1] if len(p) > 1 else 1) - 1)
                        elif final == 'C':
                            self.col += int(params or 1)
                        elif final == 'm':
                            self.sgr(params)
                        elif final == 'J':
                            self.cells = [[(' ', self.fg, self.bg)] * COLS for _ in range(ROWS)]
                        elif final == 'K':
                            for c in range(self.col, COLS):
                                self.cells[self.row][c] = (' ', self.fg, self.bg)
                    i += m.end()
                    continue
                if data[i + 1] == ord('_'):        # APC: skip to ST
                    end = data.find(b'\x1b\\', i)
                    if end < 0:
                        self.pending = data[i:]
                        return
                    i = end + 2
                    continue
                i += 2
                continue
            if b == 13:
                self.col = 0
            elif b == 10:
                self.row = min(self.row + 1, ROWS - 1)
            elif b >= 32:
                if 0 <= self.row < ROWS and 0 <= self.col < COLS:
                    self.cells[self.row][self.col] = (bytes([b]).decode('cp437'), self.fg, self.bg)
                self.col += 1
            i += 1

    def png(self, path):
        img = Image.new('RGB', (COLS * CW, ROWS * CH))
        d = ImageDraw.Draw(img)
        for r in range(ROWS):
            for c in range(COLS):
                ch, fg, bg = self.cells[r][c]
                x, y = c * CW, r * CH
                d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=bg)
                if ch == '▀':
                    d.rectangle([x, y, x + CW - 1, y + CH // 2 - 1], fill=fg)
                elif ch == '▄':
                    d.rectangle([x, y + CH // 2, x + CW - 1, y + CH - 1], fill=fg)
                elif ch == '█':
                    d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=fg)
                elif ch == '▌':
                    d.rectangle([x, y, x + CW // 2 - 1, y + CH - 1], fill=fg)
                elif ch == '▐':
                    d.rectangle([x + CW // 2, y, x + CW - 1, y + CH - 1], fill=fg)
                elif ch in '░▒▓':
                    k = {'░': 0.25, '▒': 0.5, '▓': 0.75}[ch]
                    mix = tuple(int(fg[j] * k + bg[j] * (1 - k)) for j in range(3))
                    d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=mix)
                elif ch != ' ':
                    d.text((x, y + 2), ch, fill=fg)
        img.save(path)


KEYS = {'up': b'\x1b[A', 'down': b'\x1b[B', 'right': b'\x1b[C', 'left': b'\x1b[D', 'enter': b'\r', 'esc': b'\x1b'}


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else '2'
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'test_out')
    os.makedirs(out, exist_ok=True)

    # Keys from the moment the game starts: (seconds, key); shots: (seconds, name)
    script = [(2.0, ' '), (3.5, ' '), (5.0, ' '),
              (6.5, 'up'), (7.0, 'up'), (7.5, 'up'), (8.0, 'up'), (8.5, 'up'),
              (10.0, 'enter'), (11.5, 'enter'), (13.0, 'enter')]
    for t in range(0, 20):                  # walk forward for two seconds, key repeat style
        script.append((17.0 + t * 0.1, 'up'))
    script += [(20.0, 'f'), (21.0, 'right'), (21.1, 'right'), (21.2, 'right'), (22.0, 'esc')]
    shots = [(1.0, 'a_signon'), (6.2, 'b_menu'), (9.5, 'c_menu_newgame'), (11.0, 'd_episode'),
             (12.5, 'e_difficulty'), (16.5, 'f_ingame'), (19.2, 'g_walked'), (21.6, 'h_turned'), (23.5, 'i_esc_menu')]

    pid, fd = pty.fork()
    if pid == 0:
        os.execv(DOOR, [DOOR])
    screen = Screen()
    start = time.time()
    game_started = None
    answered_cterm = picked = started = False
    si = ki = 0
    total = 0
    try:
        while True:
            r, _, _ = select.select([fd], [], [], 0.02)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                total += len(data)
                screen.feed(data)
                if not answered_cterm and b'\x1b[c' in data:
                    os.write(fd, b'\x1b[=67;84;101;114;109;1;324c')    # CTerm 1.324, like TERMinator
                    answered_cterm = True
                if not picked and b'Q = back to the BBS' in data:
                    os.write(fd, mode.encode())
                    picked = True
                if picked and not started and b'Press any key to start' in data:
                    time.sleep(0.2)
                    os.write(fd, b' ')
                    started = True
                    game_started = time.time()
            if game_started is not None:
                now = time.time() - game_started
                while ki < len(script) and now >= script[ki][0]:
                    k = script[ki][1]
                    os.write(fd, KEYS.get(k, k.encode()))
                    ki += 1
                while si < len(shots) and now >= shots[si][0]:
                    screen.png(os.path.join(out, shots[si][1] + '.png'))
                    si += 1
                if si >= len(shots):
                    break
            if time.time() - start > 90:
                print('timed out')
                break
    finally:
        elapsed = time.time() - (game_started or start)
        print(f'{total} bytes in {elapsed:.1f} s ({total / max(elapsed, 0.1) / 1024:.0f} KB/s)')
        os.kill(pid, 9)


if __name__ == '__main__':
    main()
