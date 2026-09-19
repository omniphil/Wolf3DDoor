#!/usr/bin/env python3
"""
Pretends to be TERMinator, so the door can be tested without a BBS or a terminal.

It runs the door on a pseudo-terminal, answers its TRACE commands the way TERMinator would, and checks that what the
door uploads really is wolf3d.wasm and wolf3d.pak, byte for byte, and that the player's files go both ways intact.
Run it after changing anything in the door:

    python3 test_door.py            # a terminal with TRACE: the full exchange
    python3 test_door.py --plain    # a terminal without it: the door should bow out politely
"""

import base64
import hashlib
import os
import pty
import re
import select
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DOOR = os.path.join(HERE, 'wolf3ddoor')
APC = '\x1b_'
ST = '\x1b\\'

INFO = 'TERMinator:TRACE;Info;v=1;wasm=1;audio=1;assets=1;send=1;store=1;tick=1'

# The size of file piece the real module sends (module/src/file_trace.c CHUNK, door/files.h FILES_CHUNK).
# They have to agree: a piece bigger than the door can decode is dropped, which loses the save.
SAVE_CHUNK = 3000

# A settings file this player already has on the BBS, which the door must send before the game starts
EXISTING_CFG = bytes(range(256)) * 20       # stands in for the player's config.wl1 (5 KB, two pieces)


def sha256_file(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def fields(text):
    out = {}
    for part in text.split(';'):
        if '=' in part:
            key, value = part.split('=', 1)
            out[key] = value
    return out


class FakeTerminal:
    def __init__(self, plain=False):
        self.plain = plain
        self.uploads = {}       # name -> bytes being collected ('module' or an asset hash)
        self.stored = {}        # what finished uploading, by hash
        self.started = None     # the WAD hash the module was told to play
        self.buffer = ''
        self.screen = ''
        self.received = {}      # the player's files, as the door sent them to the game
        self.order = []         # what the game was sent, in order: file names, then 'pak'

    def reply(self, fd, text):
        os.write(fd, (APC + text + ST).encode())

    def handle(self, fd, command):
        verb = command.split(';')[0]
        args = fields(command)

        if verb == 'Query':
            if not self.plain:
                self.reply(fd, INFO)
        elif verb == 'Asset':
            key = args['sha256']
            if key in self.stored:
                self.reply(fd, f'TERMinator:TRACE;Have;module=wolf3d;sha256={key}')
            else:
                self.uploads[key] = bytearray()
                self.reply(fd, f'TERMinator:TRACE;NeedAsset;module=wolf3d;sha256={key}')
        elif verb == 'Open':
            key = args['wasm']
            if key in self.stored:
                self.reply(fd, 'TERMinator:TRACE;Ready;module=wolf3d')
            else:
                self.uploads['module:' + key] = bytearray()
                self.reply(fd, f'TERMinator:TRACE;Need;module=wolf3d;wasm={key}')
        elif verb == 'Put':
            key = args.get('asset') or 'module:' + next(k[7:] for k in self.uploads if k.startswith('module:'))
            chunk = base64.b64decode(args['data'])
            assert int(args['offset']) == len(self.uploads[key]), 'chunks arrived out of order'
            self.uploads[key] += chunk
        elif verb == 'PutDone':
            key = args.get('asset') or next(k for k in self.uploads if k.startswith('module:'))
            data = bytes(self.uploads.pop(key))
            digest = hashlib.sha256(data).hexdigest()
            expect = key.split(':')[-1]
            assert digest == expect, f'what arrived does not match its hash ({digest} vs {expect})'
            self.stored[expect] = data
            if key.startswith('module:'):
                self.reply(fd, 'TERMinator:TRACE;Ready;module=wolf3d')
            else:
                self.reply(fd, f'TERMinator:TRACE;Have;module=wolf3d;sha256={expect}')
        elif verb == 'Data':
            if 'pak' in args:
                self.started = args['pak']
                self.order.append('pak')
                # The player plays for a moment, saves a game, and quits
                time.sleep(0.2)
                self.play(fd)
                self.reply(fd, 'TERMinator:TRACE;Closed;module=wolf3d;code=0')
            elif 'b64' in args:
                # Something the door sent the game: one of the player's files, a piece at a time
                message = base64.b64decode(args['b64'])
                head = message.split(b'\n', 1)[0].decode('latin-1')
                body = message.split(b'\n', 1)[1] if b'\n' in message else b''
                if head.startswith('file'):
                    f = dict(p.split('=') for p in head.split()[1:])
                    assert int(f['off']) == len(self.received.get(f['name'], b'')), 'file pieces out of order'
                    self.received.setdefault(f['name'], bytearray()).extend(body)
                    if f['name'] not in self.order:
                        self.order.append(f['name'])
        elif verb == 'Close':
            pass

    def module_says(self, fd, head, payload=b''):
        """Pretends to be the game talking to the door, which TERMinator relays as base64."""
        message = head.encode() + (b'\n' + payload if payload else b'')
        self.reply(fd, 'TERMinator:TRACE;Data;module=wolf3d;b64=' + base64.b64encode(message).decode())

    def play(self, fd):
        """The game saves, which is all the door has to handle while the player plays."""
        save = bytes(range(256)) * 25          # stands in for a saved game (spans several pieces)

        # First, a save that loses a piece on the way: the door must refuse it rather than keep a holed file
        for off in range(0, len(save), SAVE_CHUNK):
            if off == SAVE_CHUNK:
                continue                       # this piece never arrives
            self.module_says(fd, f'put name=savegam0.wl1 off={off} total={len(save)}', save[off:off + SAVE_CHUNK])
        time.sleep(0.3)

        # Then a good one, in the pieces the real game sends
        for off in range(0, len(save), SAVE_CHUNK):
            self.module_says(fd, f'put name=savegam0.wl1 off={off} total={len(save)}', save[off:off + SAVE_CHUNK])
        self.saved = save

        # And something the door must never write: a name outside the ones it keeps
        self.module_says(fd, 'put name=../../evil off=0 total=4', b'evil')
        time.sleep(0.5)

    def feed(self, fd, text):
        self.buffer += text
        while True:
            start = self.buffer.find(APC)
            if start < 0:
                self.screen += self.buffer
                self.buffer = ''
                return
            end = self.buffer.find(ST, start)
            if end < 0:
                return
            self.screen += self.buffer[:start]
            command = self.buffer[start + 2:end]
            self.buffer = self.buffer[end + 2:]
            if command.startswith('TERMinator:TRACE;'):
                self.handle(fd, command[len('TERMinator:TRACE;'):])


def run(plain=False, timeout=120):
    terminal = FakeTerminal(plain)
    primary, secondary = pty.openpty()
    door = subprocess.Popen([DOOR], stdin=secondary, stdout=secondary, stderr=subprocess.STDOUT, cwd=HERE)
    os.close(secondary)

    deadline = time.time() + timeout
    last_output = time.time()
    while time.time() < deadline:
        ready, _, _ = select.select([primary], [], [], 0.2)
        if ready:
            last_output = time.time()
            try:
                data = os.read(primary, 65536)
            except OSError:
                break
            if not data:
                break
            terminal.feed(primary, data.decode('latin-1'))
        if door.poll() is not None:
            # drain whatever is left
            while select.select([primary], [], [], 0.1)[0]:
                try:
                    data = os.read(primary, 65536)
                except OSError:
                    break
                if not data:
                    break
                terminal.feed(primary, data.decode('latin-1'))
            break
        # The door stops at its start page and at "press any key". On the start page Enter picks what it
        # recommends (TRACE when it was found); a terminal without TRACE leaves with Q instead of playing in ANSI.
        if door.poll() is None and time.time() - last_output > 1.0:
            on_menu = 'Q = back to the BBS' in terminal.screen[-400:]
            os.write(primary, b'q' if (plain and on_menu) else b'\r')
            last_output = time.time()
    else:
        door.kill()
        raise SystemExit('the door never finished')

    return door.wait(), terminal


def main():
    plain = '--plain' in sys.argv
    code, terminal = run(plain)
    screen = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', terminal.screen)

    print(f'door exit code: {code}')
    print('--- what the player saw ---')
    print('\n'.join(line for line in screen.splitlines() if line.strip()))
    print('---')

    if plain:
        assert 'NOT FOUND' in screen, 'a terminal without TRACE should see TRACE marked NOT FOUND'
        print('PASS: a terminal without TRACE sees it marked NOT FOUND and is offered the ANSI modes')
        return

    wasm = sha256_file(os.path.join(HERE, 'wolf3d.wasm'))
    pak = sha256_file(os.path.join(HERE, 'wolf3d.pak'))
    assert wasm in terminal.stored, 'the game was never uploaded'
    assert pak in terminal.stored, 'the game data was never uploaded'
    assert terminal.stored[wasm] == open(os.path.join(HERE, 'wolf3d.wasm'), 'rb').read(), 'the game arrived damaged'
    assert terminal.stored[pak] == open(os.path.join(HERE, 'wolf3d.pak'), 'rb').read(), 'the data arrived damaged'
    assert terminal.started == pak, 'the module was not told which data to use'
    assert code == 0, 'the door should exit cleanly'
    print(f'PASS: game ({len(terminal.stored[wasm]):,} bytes) and data ({len(terminal.stored[pak]):,} bytes) '
          'arrived intact, and the game was started')

    folder = os.path.join(HERE, 'saves', 'player')
    assert bytes(terminal.received.get('config.wl1', b'')) == EXISTING_CFG, 'the player\'s settings did not arrive intact'
    assert terminal.order.index('config.wl1') < terminal.order.index('pak'), 'settings must arrive before the game starts'
    print(f'PASS: the player\'s settings ({len(EXISTING_CFG):,} bytes) were sent, whole, before the game started')

    on_disk = open(os.path.join(folder, 'savegam0.wl1'), 'rb').read()
    assert on_disk == terminal.saved, 'the saved games on disk do not match what the game sent'
    print(f'PASS: saved games ({len(on_disk):,} bytes) kept for the player')
    assert not os.path.exists(os.path.join(folder, 'savegam0.wl1.new')), 'a half-written file was left behind'
    assert not any('evil' in n for root, _, names in os.walk(os.path.join(HERE, '..')) for n in names), \
        'the door wrote a file it should never accept'
    print('PASS: a save with a missing piece was refused, and an unknown file name was ignored')


def prepare():
    """A fresh player folder, holding a settings file from an 'earlier call'."""
    folder = os.path.join(HERE, 'saves', 'player')
    for name in ('savegam0.wl1', 'savegam0.wl1.bak', 'savegam0.wl1.new', 'config.wl1', 'ansi-config.wl1', 'display.cfg'):
        try:
            os.remove(os.path.join(folder, name))
        except FileNotFoundError:
            pass
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'config.wl1'), 'wb') as f:
        f.write(EXISTING_CFG)


if __name__ == '__main__':
    if '--plain' not in sys.argv:
        prepare()
    main()
