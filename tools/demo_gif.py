#!/usr/bin/env python3
"""Render a demo GIF of the wrist-flick wave or the rainbow startup (needs Pillow).

Simulates the flap state machine from src/c/main.c frame by frame at 30 fps
and draws each frame with the same tile renderer as glyph_compile.py's preview.

Usage: demo_gif.py OUT.gif [--rainbow] [--scale N]
"""
import sys

from PIL import Image

import glyph_compile as gc

COLS, ROWS, GAP_X, GAP_Y = 8, 4, 3, 10
SCREEN_W, SCREEN_H = 144, 168
BOARD_W = COLS * gc.TILE_W + (COLS - 1) * GAP_X
BOARD_H = ROWS * gc.TILE_H + (ROWS - 1) * GAP_Y
FRAMES_PER_STEP = 3
# GIF delays are in 1/100 s; 30+30+40 ms keeps each 3-frame flap step at 100 ms.
STEP_DELAYS = (30, 30, 40)
HOLD_MS = 1200
STARTUP_HOLD_MS = 500
RAINBOW_KEYS = 'roygbv'

BOARD = ['09:41 AM', '11/04/26', '080%', '8421*']


class Cell:
    def __init__(self, flap):
        self.cur = self.target = flap
        self.laps = self.delay = 0
        self.flipping = False

    def moving(self):
        return self.cur != self.target or self.laps > 0


def rainbow(cells):
    # Same as set_rainbow(): flaps start on the rainbow, the board text is their target.
    for r, row in enumerate(cells):
        for col, c in enumerate(row):
            c.cur = gc.FLAPS.index(RAINBOW_KEYS[(col - r) % len(RAINBOW_KEYS)])


def flick(cells):
    # Same as tap_handler(): each filled flap trails its filled left neighbour by one step.
    for row in cells:
        wave = 0
        for c in row:
            if c.target == 0:
                continue
            if c.laps == 0:
                c.laps, c.delay = 1, wave
            wave += 1


def step(cells, phase):
    # Same as anim_step(); returns whether anything is still animating.
    busy = False
    for row in cells:
        for c in row:
            if phase == 0 and c.moving():
                if c.delay > 0:
                    c.delay -= 1
                else:
                    c.flipping = True
            elif phase == 1 and c.flipping:
                c.flipping = False
                c.cur = (c.cur + 1) % len(gc.FLAPS)
                if c.cur == c.target and c.laps > 0:
                    c.laps -= 1
            busy |= c.moving() or c.flipping
    return busy


def render(cells, glyphs, scale):
    im = Image.new('RGB', (SCREEN_W, SCREEN_H), gc.BLACK)
    px = im.load()
    x0, y0 = (SCREEN_W - BOARD_W) // 2, (SCREEN_H - BOARD_H) // 2
    for r, row in enumerate(cells):
        for col, c in enumerate(row):
            nxt = gc.FLAPS[(c.cur + 1) % len(gc.FLAPS)]
            gc.draw_tile(px, x0 + col * (gc.TILE_W + GAP_X), y0 + r * (gc.TILE_H + GAP_Y),
                         gc.FLAPS[c.cur], nxt, glyphs, flipping=c.flipping)
    return im.resize((SCREEN_W * scale, SCREEN_H * scale), Image.NEAREST)


def main():
    out = sys.argv[1]
    scale = int(sys.argv[sys.argv.index('--scale') + 1]) if '--scale' in sys.argv else 2
    glyphs = gc.load()
    cells = [[Cell(gc.FLAPS.index(text[i] if i < len(text) else ' ')) for i in range(COLS)]
             for text in BOARD]

    start_rainbow = '--rainbow' in sys.argv
    if start_rainbow:
        rainbow(cells)
    else:
        flick(cells)
    frames = [render(cells, glyphs, scale)]
    delays = [STARTUP_HOLD_MS if start_rainbow else HOLD_MS]
    frame, busy = 0, True
    while busy:
        busy = step(cells, frame % FRAMES_PER_STEP)
        img, ms = render(cells, glyphs, scale), STEP_DELAYS[frame % FRAMES_PER_STEP]
        if img.tobytes() == frames[-1].tobytes():
            delays[-1] += ms   # merge identical frames (the second rest frame)
        else:
            frames.append(img)
            delays.append(ms)
        frame += 1
    delays[-1] += HOLD_MS

    frames[0].save(out, save_all=True, append_images=frames[1:], duration=delays,
                   loop=0, optimize=True, disposal=1)
    print('wrote %s: %d frames, %.1f s animation' % (out, len(frames), frame * 100 / 3 / 1000))


if __name__ == '__main__':
    main()
