# @file      ducks.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Lucky Duck's ducks (devplan/luckyduck.md §3): one duck drawn from parts in a 128-unit frame,
# facing right, its style (the colours, what it wears) and its pose (a step, the wings, the beak,
# the eyes) as parameters: Lucky's poses, the ducklings, the players, the courts' heads. Lit from
# the upper left: each part shaded round, layered feathers on the wing and tail, an eye with an
# iris and two highlights, a beak with its mouth line.

import math

from draw import (Picture, arc_points, capsule, circle, ellipse, lit, mix, paint, polygon, shade, stroke)

OUTLINE = (0x3A, 0x24, 0x10)
EYE = (0x1C, 0x14, 0x0E)
WHITE = (0xFF, 0xFF, 0xFF)
ORANGE = (0xFF, 0x8C, 0x1A)
PINK = (0xFF, 0x8A, 0x8A)
GREEN = (0x1E, 0x9A, 0x50)
GREEN_DARK = (0x0E, 0x5E, 0x30)
RED = (0xD0, 0x20, 0x30)

STYLES = {
    "lucky": dict(body=(0xFF, 0xD4, 0x3C), belly=(0xFF, 0xE8, 0x8C), wing=(0xF6, 0xB4, 0x24), beak=ORANGE,
                  feet=ORANGE, iris=(0x4A, 0x2C, 0x12), visor=True, bowtie=RED),
    "duckling": dict(body=(0xFF, 0xE2, 0x58), belly=(0xFF, 0xF0, 0xA8), wing=(0xF8, 0xC8, 0x40), beak=ORANGE,
                     feet=ORANGE, iris=(0x3A, 0x24, 0x10), fluff=True),
    "mallard": dict(body=(0xB0, 0x9C, 0x84), belly=(0x8E, 0x5C, 0x3C), wing=(0x86, 0x76, 0x66),
                    head=(0x22, 0x80, 0x40), ring=WHITE, beak=(0xEC, 0xCC, 0x34), feet=ORANGE,
                    iris=(0x3A, 0x24, 0x10), speculum=(0x34, 0x54, 0xD0)),
    "pekin": dict(body=(0xF8, 0xF4, 0xEA), belly=(0xFF, 0xFF, 0xFA), wing=(0xE6, 0xE0, 0xD2), beak=ORANGE,
                  feet=ORANGE, iris=(0x2A, 0x50, 0x90), lashes=True),
    "wood": dict(body=(0xA0, 0x6C, 0x46), belly=(0xEA, 0xDA, 0xB2), wing=(0x5C, 0x4A, 0x3E),
                 head=(0x3C, 0x6E, 0x8E), ring=WHITE, beak=(0xDA, 0x42, 0x32), feet=(0xE2, 0xA2, 0x42),
                 iris=(0xC0, 0x30, 0x20)),
    "black": dict(body=(0x4E, 0x44, 0x3E), belly=(0x6E, 0x60, 0x56), wing=(0x3A, 0x32, 0x2E),
                  head=(0x60, 0x56, 0x4E), beak=(0x9C, 0xAA, 0x62), feet=(0xD2, 0x82, 0x42),
                  iris=(0x3A, 0x24, 0x10)),
    "rubber": dict(body=(0xFF, 0xE0, 0x20), belly=(0xFF, 0xEC, 0x6C), wing=(0xFF, 0xCA, 0x14),
                   beak=(0xFF, 0x62, 0x12), feet=None, iris=(0x10, 0x10, 0x10), glossy=True),
}

REST = dict(bob=0.0, lean=0.0, wing=0.0, beak=0.0, eyes="open", step=None, head=0.0)


def pose(**changes):
    p = dict(REST)
    p.update(changes)
    return p


def feather(cx, cy, length, width, angle):
    """A feather: a long ellipse centred on (cx, cy) turned by @p angle degrees."""
    return ellipse(cx, cy, length / 2, width / 2).at(turn=angle, cx=cx, cy=cy)


class Painter:
    """Paints local shapes (a 128-unit frame) into a picture through a placing transform."""

    def __init__(self, pic, ox, oy, size, mirror=False, lean=0.0, cx=64.0, cy=82.0, outline=2.0):
        self.pic = pic
        self.k = size / 128.0
        self.mirror = mirror
        self.lean = lean
        self.cx, self.cy = cx, cy
        self.dx = ox + cx * self.k - cx
        self.dy = oy + cy * self.k - cy
        self.width = outline

    def aim(self, lx, ly, tx, ty):
        """Moves the duck so its local point (@p lx, @p ly) lands on (@p tx, @p ty)."""
        mx = 2 * self.cx - lx if self.mirror else lx
        self.dx = tx - (self.cx + self.k * (mx - self.cx))
        self.dy = ty - (self.cy + self.k * (ly - self.cy))

    def place(self, shape):
        return shape.at(self.dx, self.dy, self.lean, self.k, self.cx, self.cy, self.mirror)

    def fill(self, shape, rgb):
        paint(self.pic, self.place(shape), rgb)

    def part(self, shape, rgb, width=None, shaded=True, glow=0.22, rim=0.32):
        """@p shape outlined, then filled (lit round unless @p shaded is false)."""
        w = self.width if width is None else width
        placed = self.place(shape)
        if w > 0:
            paint(self.pic, self.place(shape.grow(w)), OUTLINE)
        paint(self.pic, placed, lit(placed, rgb, glow=glow, rim=rim, edge=6 * self.k) if shaded else rgb)

    def shine(self, shape, rgb, amount):
        """A highlight: @p shape in @p rgb lightened by @p amount."""
        paint(self.pic, self.place(shape), mix(rgb, WHITE, amount))


def head_shape():
    return circle(86, 46, 21)


def draw_eye(p, style, eyes, t):
    if eyes == "open":
        p.part(t(ellipse(91.5, 40, 7.2, 8.4)), WHITE, 1.2, glow=0.05, rim=0.18)
        iris = style.get("iris", EYE)
        p.fill(t(circle(93.6, 41.2, 4.8)), iris)
        p.fill(t(circle(93.6, 42.6, 3.4) & circle(93.6, 41.2, 4.8)), mix(iris, WHITE, 0.25))
        p.fill(t(circle(93.8, 41.3, 2.7)), EYE)
        p.fill(t(circle(95.6, 38.6, 1.7)), WHITE)
        p.fill(t(circle(92.0, 43.6, 0.8)), WHITE)
        p.fill(t(stroke(arc_points(91.5, 40.5, 7.6, 8.6, -160, -20), 0.9)), OUTLINE)  # the lid
        if style.get("lashes"):
            for a, b in (((86.5, 33.5), (83.5, 29.5)), ((90, 32), (89.2, 27.2)), ((93.8, 32.2), (94.6, 27.6))):
                p.fill(t(capsule(a[0], a[1], b[0], b[1], 0.9)), EYE)
    elif eyes == "closed":
        p.fill(t(stroke(arc_points(91.5, 39, 6, 4.2, 15, 165), 1.3)), EYE)
    elif eyes == "happy":
        p.fill(t(stroke(arc_points(91.5, 43.5, 6, 5, -160, -20), 1.5)), EYE)
    elif eyes == "sad":
        p.part(t(ellipse(91.5, 41.5, 7, 7)), WHITE, 1.2, glow=0.05, rim=0.18)
        p.fill(t(circle(92.5, 43.5, 3.2)), EYE)
        p.fill(t(circle(93.6, 42.2, 1.0)), WHITE)
        p.fill(t(polygon([(83, 31), (101, 37), (101, 31)])), style.get("head", style["body"]))  # the drooping lid
        p.fill(t(stroke([(84, 33.5), (100, 38.5)], 1.2)), OUTLINE)


def draw_head(p, style, pose_, neck=True):
    """The head (and the neck down to the body), its face and what it wears."""
    head_colour = style.get("head", style["body"])
    tilt = pose_.get("head", 0.0)

    def t(s):
        return s.at(turn=tilt, cx=80, cy=62) if tilt else s

    shape = head_shape() | ellipse(80, 62, 13, 14) if neck else head_shape()
    p.part(t(shape), head_colour)
    p.shine(t(ellipse(79, 34, 8, 5.5).at(turn=-25, cx=79, cy=34) & head_shape()), head_colour, 0.35)
    if style.get("ring"):
        p.fill(t(capsule(68, 66, 94, 63, 2.2) & shape), style["ring"])
    if style.get("fluff"):  # a tuft on top
        for a, b in (((82, 26), (78, 17)), ((86, 25), (87, 15)), ((90, 26), (96, 18))):
            p.part(t(capsule(a[0], a[1], b[0], b[1], 1.7)), head_colour, 1.3, shaded=False)
    if style.get("glossy"):
        p.shine(t(ellipse(78, 35, 7, 4).at(turn=-30, cx=78, cy=35)), head_colour, 0.75)
    p.fill(t(ellipse(96, 54, 5, 3.2)), mix(head_colour, PINK, 0.45))  # the cheek
    draw_eye(p, style, pose_.get("eyes", "open"), t)
    # The beak: the mouth when open, the lower half, the upper with its line and shine.
    beak = style["beak"]
    open_ = pose_.get("beak", 0.0)
    if open_ > 0:
        p.fill(t(polygon([(101, 52), (118, 53), (116, 61), (101, 61)]).at(turn=open_ / 2, cx=101, cy=53)),
               (0x7A, 0x1C, 0x24))
        p.fill(t(ellipse(110, 57, 4, 2).at(turn=open_ / 2, cx=101, cy=53)), (0xD8, 0x50, 0x60))  # a tongue
    p.part(t(polygon([(100, 53.5), (117, 54), (115.5, 58.5), (100, 60)]).at(turn=open_, cx=101, cy=54)),
           shade(beak, 0.85), 1.6)
    upper = polygon([(98.5, 43), (110, 44.2), (119, 46.8), (124, 50.4), (121.5, 54), (110, 55.4), (98.5, 55.8)])
    p.part(t(upper), beak, 1.8, glow=0.3, rim=0.25)
    p.shine(t(stroke([(103, 46.4), (114, 47.6), (119, 49.4)], 1.0)), beak, 0.55)
    p.fill(t(stroke([(101, 55.2), (112, 54.8), (121, 54)], 0.6)), shade(beak, 0.55))
    p.fill(t(ellipse(110.5, 48.8, 1.4, 0.8)), shade(beak, 0.5))
    # What it wears on its head.
    if style.get("visor"):
        p.part(t(stroke([(66.5, 38.5), (77, 29.5), (97, 28.2)], 3.0)), GREEN_DARK, 1.8, shaded=False)
        brim = polygon([(91, 25.5), (110, 27.6), (117, 30.4), (114, 34.6), (92, 33)])
        p.part(t(brim), GREEN, 1.8, glow=0.35)
        p.shine(t(polygon([(95, 27.6), (110, 29.4), (113, 30.8), (95, 30.2)])), GREEN, 0.45)


def draw_duck(pic, ox, oy, size, style_name, pose_=None, mirror=False):
    """A whole duck in the @p size square at (@p ox, @p oy), facing right (left if @p mirror)."""
    style = STYLES[style_name]
    pose_ = pose_ or REST
    p = Painter(pic, ox, oy + pose_.get("bob", 0.0) * size / 128.0, size, mirror, pose_.get("lean", 0.0))
    body_c, wing_c = style["body"], style["wing"]
    # The feet, behind: walking, each swings on a circle (lifted on its way forward).
    if style.get("feet") is not None:
        step = pose_.get("step")
        for i in (0, 1):
            if step is None:
                fx, lift = 54 + i * 16, 0.0
            else:
                ph = step * math.pi / 2 + i * math.pi
                fx, lift = 62 + 9 * math.cos(ph), max(0.0, math.sin(ph)) * 6
            fy = 112 - lift
            foot_c = style["feet"] if i == 1 else shade(style["feet"], 0.85)  # the far one darker
            p.part(capsule(fx, 100, fx + 1, fy, 2.4), foot_c, 1.5, shaded=False)
            web = polygon([(fx - 8, fy + 2), (fx + 3, fy - 2.5), (fx + 13.5, fy + 0.5), (fx + 12, fy + 3.5),
                           (fx + 7, fy + 3), (fx + 3, fy + 5), (fx - 2, fy + 4.2), (fx - 7.5, fy + 5)])
            p.part(web, foot_c, 1.5, glow=0.2, rim=0.2)
            for tx in (3, 8):
                p.fill(stroke([(fx + 1, fy), (fx + tx, fy + 3.5)], 0.5), shade(foot_c, 0.7))
    # The tail: three feathers fanned up behind.
    for ang, ln, cx, cy in ((-58, 22, 22, 60), (-38, 24, 20, 66), (-18, 20, 22, 72)):
        p.part(feather(cx, cy, ln, 9, ang), shade(body_c, 0.95), 1.6)
    # The body, its belly, the chest's feathers.
    body = ellipse(62, 82, 37, 26)
    p.part(body, body_c)
    p.fill(ellipse(73, 93, 25, 14) & body, lit(p.place(ellipse(73, 93, 25, 14)), style["belly"], glow=0.15, rim=0.0))
    for k in range(3):
        p.fill(stroke(arc_points(84 + k * 3.5, 74 + k * 6, 4.2, 2.4, 20, 160), 0.55) & body, shade(body_c, 0.86))
    if style.get("glossy"):
        p.shine(ellipse(46, 70, 13, 5).at(turn=-15, cx=46, cy=70), body_c, 0.75)
    else:
        p.shine(ellipse(52, 66, 14, 5).at(turn=-12, cx=52, cy=66) & body, body_c, 0.3)
    # The wing: a base and its long feathers, lifted together about the shoulder.
    a = -8 + pose_.get("wing", 0.0)

    def w(s):
        return s.at(turn=a, cx=70, cy=72)

    # The primaries reach back past the wing towards the tail, the lowest furthest.
    for k, (fx, fy, ln, ang) in enumerate(((42, 82, 30, -10), (45, 79, 28, -16), (48, 76, 24, -22))):
        p.part(w(feather(fx, fy, ln, 7.5, ang)), shade(wing_c, 0.8 + 0.07 * k), 1.3)
    wing = ellipse(60, 79, 18, 12)
    p.part(w(wing), wing_c)
    if style.get("speculum"):
        p.fill(w(ellipse(50, 83, 7, 3.4) & wing), style["speculum"])
    for k in range(3):  # coverts' edges
        p.fill(w(stroke(arc_points(52 + k * 6, 82 - k * 2, 5, 3, 20, 160), 0.6) & wing), shade(wing_c, 0.75))
    p.shine(w(ellipse(62, 73, 9, 3.4) & wing), wing_c, 0.3)
    draw_head(p, style, pose_)
    if style.get("bowtie"):
        bt = style["bowtie"]
        p.part(polygon([(88, 69), (78.5, 62), (77, 69), (78.5, 76)]), bt, 1.5)
        p.part(polygon([(88, 69), (97.5, 62), (99, 69), (97.5, 76)]), bt, 1.5)
        for sx in (-1, 1):
            p.fill(stroke([(88 + sx * 3, 68), (88 + sx * 8, 66)], 0.5), shade(bt, 0.65))
            p.fill(stroke([(88 + sx * 3, 70), (88 + sx * 8, 72.5)], 0.5), shade(bt, 0.65))
        p.part(ellipse(88, 69, 3, 3.6), shade(bt, 0.85), 1.3)


# ---- The frames the program uses

LUCKY_POSES = {
    "LUCKY_IDLE": pose(),
    "LUCKY_BLINK": pose(eyes="closed"),
    "LUCKY_WALK0": pose(step=0, bob=-1, head=-2, wing=2),
    "LUCKY_WALK1": pose(step=1, bob=1.5, lean=-3, head=1, wing=-1),
    "LUCKY_WALK2": pose(step=2, bob=-1, head=-2, wing=2),
    "LUCKY_WALK3": pose(step=3, bob=1.5, lean=3, head=1, wing=-1),
    "LUCKY_QUACK": pose(beak=30, wing=20, head=-8, bob=-1),
    "LUCKY_CHEER": pose(eyes="happy", beak=18, wing=60, bob=-8, step=1),
    "LUCKY_SAD": pose(eyes="sad", wing=-10, head=12, bob=3),
    "LUCKY_SLEEP": pose(eyes="closed", head=16, bob=4),
    "LUCKY_THINK": pose(head=-10, wing=35),
    "LUCKY_DEAL": pose(wing=40, beak=6),
}

DUCKLING_POSES = {
    "DUCKLING_WALK0": pose(step=0, bob=-2),
    "DUCKLING_WALK1": pose(step=1, bob=2, lean=-4),
    "DUCKLING_WALK2": pose(step=2, bob=-2),
    "DUCKLING_WALK3": pose(step=3, bob=2, lean=4),
}


# The bot ducks at the Blackjack table (devplan/luckyduck.md §16): their kinds and names, and
# the poses a player needs.
BOTS = [("mallard", "MALLORY"), ("pekin", "PEARL"), ("wood", "WOODY"), ("rubber", "SQUEAKY"), ("black", "INKY")]
BOT_POSES = {
    "IDLE": pose(),
    "THINK": pose(head=-10, wing=35, eyes="open"),
    "HAPPY": pose(eyes="happy", beak=16, wing=55, bob=-6),
    "SAD": pose(eyes="sad", wing=-10, head=12, bob=3),
}


def bot(name, size=84):
    """BOT_<KIND>_<POSE>."""
    _, kind, pose_name = name.split("_", 2)
    return frame(kind.lower(), BOT_POSES[pose_name], size)


def frame(style, pose_, size):
    pic = Picture(size, size)
    draw_duck(pic, 0, 0, size, style, pose_)
    return pic


def lucky(name, size=128):
    if name in CUP_POSES:
        return lucky_cup(name, size)
    return frame("lucky", LUCKY_POSES[name], size)


# Lucky with the dice cup (devplan/luckyduck.md §18): shaking it (two poses) and throwing. Each:
# the duck's pose, and the cup's middle and turn in the duck's frame.
CUP_POSES = {
    "LUCKY_SHAKE0": (pose(wing=55, head=-6, beak=8), (100, 70), -18),
    "LUCKY_SHAKE1": (pose(wing=30, head=4, bob=-2), (106, 78), 14),
    "LUCKY_THROW": (pose(wing=70, beak=22, eyes="happy", head=-4), (100, 84), 105),
}
LEATHER = (0x8A, 0x4A, 0x1E)
BRASS = (0xE0, 0xB0, 0x40)


def draw_cup(p, cx, cy, turn):
    """The dice cup: tapered leather with brass bands, its mouth up before it is turned."""
    def t(s):
        return s.at(turn=turn, cx=cx, cy=cy)

    body = polygon([(cx - 14, cy - 18), (cx + 14, cy - 18), (cx + 11, cy + 18), (cx - 11, cy + 18)])
    p.part(t(body), LEATHER, 1.8)
    for y, w in ((cy - 15, 14), (cy + 14, 11.4)):
        p.part(t(polygon([(cx - w, y - 2.4), (cx + w, y - 2.4), (cx + w - 0.4, y + 2.4), (cx - w + 0.4, y + 2.4)])),
               BRASS, 1.0, glow=0.35)
    p.part(t(ellipse(cx, cy - 18, 14, 4)), shade(LEATHER, 0.45), 1.4, shaded=False)  # the mouth
    p.shine(t(polygon([(cx - 9, cy - 10), (cx - 6, cy - 10), (cx - 5, cy + 10), (cx - 7.5, cy + 10)])), LEATHER, 0.35)


def lucky_cup(name, size):
    pose_, (cx, cy), turn = CUP_POSES[name]
    pic = Picture(size, size)
    draw_duck(pic, 0, 0, size, "lucky", pose_)
    p = Painter(pic, 0, pose_.get("bob", 0.0) * size / 128.0, size, False, pose_.get("lean", 0.0))
    draw_cup(p, cx, cy, turn)
    return pic


def duckling(name, size=56):
    return frame("duckling", DUCKLING_POSES[name], size)


def ground_shadow():
    """The shadow under a duck (a shade's shape: only its pixels count)."""
    pic = Picture(96, 20)
    paint(pic, ellipse(48, 10, 46, 9), (0, 0, 0))
    return pic
