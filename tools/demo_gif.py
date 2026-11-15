#!/usr/bin/env python3
"""Render a demo GIF of the wrist-flick wave or the rainbow startup (needs Pillow).

Simulates the flap state machine from src/c/main.c frame by frame at 30 fps
and draws each frame with the same tile renderer as glyph_compile.py's preview.

Usage: demo_gif.py OUT.gif [--rainbow] [--scale N] [--platform basalt|emery|gabbro]
"""
import sys

from PIL import Image, ImageDraw

import glyph_compile as gc

FRAMES_PER_STEP = 3
# GIF delays are in 1/100 s; 30+40+30 ms keeps each 3-frame flap step at 100 ms.
STEP_DELAYS = (30, 40, 30)
HOLD_MS = 1200
STARTUP_HOLD_MS = 500
RAINBOW_KEYS = 'roygbv'
TEXT_COLS = 8

INFO = ['09:41 AM', '11/04/26', '080%', '8421*']


class Layout:
    """One platform's board; must match the layout in src/c/main.c."""

    def __init__(self, screen, geo, row_len, gap_x, gap_y, text, rnd=False):
        self.screen_w, self.screen_h = screen
        self.geo, self.row_len, self.gap_x, self.gap_y = geo, row_len, gap_x, gap_y
        self.text, self.round = text, rnd
        self.max_cols = max(row_len)
        self.board_h = len(row_len) * geo.tile_h + (len(row_len) - 1) * gap_y


LAYOUTS = {
    'basalt': Layout((144, 168), gc.BASALT, [8] * 4, 3, 10, INFO),
    'emery': Layout((200, 228), gc.HD, [8] * 4, 4, 13, INFO),
    # The default round board: blank custom rows around time, date and battery.
    'gabbro': Layout((260, 260), gc.HD, [3, 8, 10, 8, 3], 4, 6, [''] + INFO[:3] + [''], rnd=True),
}


class Cell:
    def __init__(self, flap):
        self.cur = self.target = flap
        self.laps = self.delay = 0
        self.flip = gc.FLIP_NONE

    def moving(self):
        return self.cur != self.target or self.laps > 0


def board(layout):
    # Same as set_mode_rows(): text is centred on rows wider than any text.
    cells = []
    for n, text in zip(layout.row_len, layout.text):
        pad = (n - TEXT_COLS) // 2 if n > TEXT_COLS else 0
        cells.append([Cell(gc.FLAPS.index(text[i - pad] if 0 <= i - pad < len(text) else ' '))
                      for i in range(n)])
    return cells


def rainbow(cells, layout):
    # Same as set_rainbow(): flaps start on the rainbow, the board text is their target.
    for r, row in enumerate(cells):
        offset = (layout.max_cols - len(row)) // 2
        for col, c in enumerate(row):
            c.cur = gc.FLAPS.index(RAINBOW_KEYS[(col + offset - r) % len(RAINBOW_KEYS)])


def flick(cells, layout=None):
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
                    c.flip = gc.FLIP_TOP
            elif phase == 1 and c.flip != gc.FLIP_NONE:
                c.flip = gc.FLIP_BOTTOM
            elif phase == 2 and c.flip != gc.FLIP_NONE:
                c.flip = gc.FLIP_NONE
                c.cur = (c.cur + 1) % len(gc.FLAPS)
                if c.cur == c.target and c.laps > 0:
                    c.laps -= 1
            busy |= c.moving() or c.flip != gc.FLIP_NONE
    return busy


def render(cells, glyphs, scale, layout=LAYOUTS['basalt']):
    geo = layout.geo
    im = Image.new('RGB', (layout.screen_w, layout.screen_h), gc.BLACK)
    px = im.load()
    y0 = (layout.screen_h - layout.board_h) // 2
    for r, row in enumerate(cells):
        n = len(row)
        x0 = (layout.screen_w - (n * geo.tile_w + (n - 1) * layout.gap_x)) // 2
        for col, c in enumerate(row):
            nxt = gc.FLAPS[(c.cur + 1) % len(gc.FLAPS)]
            gc.draw_tile(px, x0 + col * (geo.tile_w + layout.gap_x), y0 + r * (geo.tile_h + layout.gap_y),
                         gc.FLAPS[c.cur], nxt, glyphs, flip=c.flip, geo=geo)
    if layout.round:
        mask = Image.new('L', im.size, 0)
        ImageDraw.Draw(mask).ellipse((0, 0, layout.screen_w - 1, layout.screen_h - 1), fill=255)
        im = Image.composite(im, Image.new('RGB', im.size, gc.BLACK), mask)
    return im.resize((layout.screen_w * scale, layout.screen_h * scale), Image.NEAREST)


def main():
    out = sys.argv[1]
    scale = int(sys.argv[sys.argv.index('--scale') + 1]) if '--scale' in sys.argv else 2
    name = sys.argv[sys.argv.index('--platform') + 1] if '--platform' in sys.argv else 'basalt'
    layout = LAYOUTS[name]
    glyphs = gc.load(layout.geo)
    cells = board(layout)

    start_rainbow = '--rainbow' in sys.argv
    if start_rainbow:
        rainbow(cells, layout)
    else:
        flick(cells)
    frames = [render(cells, glyphs, scale, layout)]
    delays = [STARTUP_HOLD_MS if start_rainbow else HOLD_MS]
    frame, busy = 0, True
    while busy:
        busy = step(cells, frame % FRAMES_PER_STEP)
        img, ms = render(cells, glyphs, scale, layout), STEP_DELAYS[frame % FRAMES_PER_STEP]
        if img.tobytes() == frames[-1].tobytes():
            delays[-1] += ms   # merge identical frames
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
