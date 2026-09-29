#!/usr/bin/env python3
"""Draws the Frequency Chart shown in Tools (freq_chart.png, HD and FHD).

An original drawing: the band frequencies are plain technical data (the
standard analog 5.8 GHz band plans A, B, E, F/I and R, and the digital D, J, O
and Q channel plans of the Walksnail / DJI systems). Dark background,
Montserrat (the UI font, in utilities/font).

    python3 utilities/gen_freq_chart.py OUT_DIR    # writes OUT_DIR/freq_chart_{hd,fhd}.png

Then shrink them (64 colors keeps the band hues right):
    pngquant --quality=85-100 --colors 64 --speed 1 --strip -o freq_chart.png freq_chart_hd.png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = os.path.join(os.path.dirname(__file__), "font", "Montserrat-Medium.ttf")

BG = (14, 15, 18)
TEXT = (232, 232, 236)
DIM = (140, 143, 152)
GRID = (34, 36, 42)
GRID_MAJOR = (60, 63, 72)

F_MIN, F_MAX = 5630, 5960


def numbered(letter, freqs):
    return [("%s%d" % (letter, i + 1), f) for i, f in enumerate(freqs)]


# key, row label, colour, descriptor, [(channel name, MHz), ...]
BANDS = [
    ("A", "A", (64, 224, 208), "Boscam A",
     numbered("A", [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725])),
    ("B", "B", (181, 230, 29), "Boscam B",
     numbered("B", [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866])),
    ("E", "E", (255, 160, 64), "Foxtech / DJI",
     numbered("E", [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945])),
    ("F", "F/I", (160, 112, 255), "FatShark / ImmersionRC",
     numbered("F", [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880])),
    ("R", "R", (255, 92, 122), "Raceband",
     numbered("R", [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917])),
    ("D", "D", (84, 128, 240), "Walksnail / DJI V1, 25 MHz",
     numbered("D", [5660, 5695, 5735, 5770, 5805, 5878, 5914, 5839])),
    ("J", "J", (140, 186, 255), "Walksnail / DJI V1, 50 MHz",
     numbered("J", [5695, 5770, 5878])),
    ("O", "O", (52, 178, 102), "DJI O3, 10 or 20 MHz",
     numbered("O", [5669, 5705, 5768, 5804, 5839, 5876, 5912])),
    ("Q", "Q", (118, 204, 150), "DJI O3, 40 MHz",
     numbered("Q", [5677, 5794, 5902])),
]

SS = 2  # supersampling for smooth edges


def mix(c, bg, a):
    return tuple(int(bg[i] + (c[i] - bg[i]) * a) for i in range(3))


def render(w, h):
    s = w / 1920.0
    W, H = w * SS, h * SS

    def px(v):
        return int(round(v * s * SS))

    def font(size):
        return ImageFont.truetype(FONT, max(px(size), 12 * SS))

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    x0, x1 = px(300), px(1860)
    y_axis = px(132)

    def fx(f):
        return x0 + (f - F_MIN) * (x1 - x0) // (F_MAX - F_MIN)

    rows = len(BANDS)
    y_rows = px(150)
    pitch = px(96)
    y_end = y_rows + rows * pitch

    d.text((px(60), px(34)), "FPV FREQUENCY REFERENCE", font=font(46), fill=TEXT)
    d.text((px(1860), px(50)), "MHz", font=font(30), fill=DIM, anchor="ra")

    # axis: faint line every 10 MHz, stronger and labelled every 50
    for f in range(5630, F_MAX + 1, 10):
        major = f % 50 == 0
        d.line([(fx(f), y_axis + (0 if major else px(10))), (fx(f), y_end)],
               fill=GRID_MAJOR if major else GRID, width=max(1, px(1)))
    for f in range(5650, 5951, 50):
        d.text((fx(f), y_axis - px(8)), str(f), font=font(28), fill=TEXT, anchor="ms")

    def channel(cx, y, name, freq, color):
        bw, bh = px(64), px(44)
        box = [cx - bw // 2, y, cx + bw // 2, y + bh]
        d.rounded_rectangle(box, radius=px(9), fill=mix(color, BG, 0.28), outline=color,
                            width=max(2, px(2)))
        d.text((cx, y + bh // 2), name, font=font(26), fill=TEXT, anchor="mm")
        d.text((cx, y + bh + px(4)), str(freq), font=font(18), fill=DIM, anchor="ma")

    for r, (_key, label, color, desc, channels) in enumerate(BANDS):
        y = y_rows + r * pitch
        d.text((px(60), y + px(2)), label, font=font(46), fill=color)
        d.text((px(60), y + px(58)), desc, font=font(15), fill=DIM)
        for name, f in channels:
            channel(fx(f), y + px(6), name, f, color)

    d.text((px(60), y_end + px(22)),
           "Markers sit at each channel's centre frequency. Channels are drawn the same size, whatever their real width.",
           font=font(20), fill=DIM)

    return img.resize((w, h), Image.LANCZOS)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    render(1280, 720).save(os.path.join(out, "freq_chart_hd.png"))
    render(1920, 1080).save(os.path.join(out, "freq_chart_fhd.png"))
    print("ok")
