"""Generate the soft-skin SatDump themes from the colour palettes in palettes.json.

Usage:
    python3 -I -B tools/theme_gen/gen.py resources/themes          # write the 13 theme files
    python3 -I -B tools/theme_gen/gen.py --check resources/themes  # verify the committed files

The palettes are adapted from the built-in themes of T3 Code (MIT licence, see
LICENSE-T3Code.txt). Every theme turns on SatDump's soft skin via its "Soft" block.
"""
import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okl import parse, hx  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
NAMES = {"soft": "Soft", "blossom": "Blossom", "grove": "Grove", "ocean": "Ocean", "ember": "Ember", "iris": "Iris"}
OLED_NAME = "Soft OLED"

# Status/extra hues the palettes have no token for (Tailwind 400 shades on dark, 600-700 on light)
EXTRA = {
    "dark": {"green": "#10B981", "yellow": "#FBBF24", "cyan": "#2DD4BF", "fuchsia": "#C084FC", "magenta": "#FB7185",
             "lavender": "#A5B4FC", "light_green": "#A3E635", "light_cyan": "#7DD3FC"},
    "light": {"green": "#059669", "yellow": "#B45309", "cyan": "#0F766E", "fuchsia": "#9333EA", "magenta": "#BE123C",
              "lavender": "#4F46E5", "light_green": "#4D7C0F", "light_cyan": "#0369A1"},
}
OLED_LINE = (0x2A, 0x2A, 0x2A, 255)  # borders on pure black: about 1.5:1, still quiet


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3)) + (255,)


def alpha(c, a):
    return c[:3] + (a,)


def build(c, light, oled=False):
    P = {k: parse(v) for k, v in c.items()}
    if oled:
        for k in ("canvas", "chrome", "toolbar", "surface", "surfaceRaised", "sidebar"):
            P[k] = (0, 0, 0, 255)
        P["border"] = OLED_LINE
    bg, fg = P["canvas"], P["text"]
    m = lambda t: mix(bg, fg, t)  # noqa: E731
    card = P["surface"] if P["surface"][:3] != bg[:3] else mix(bg, P["surfaceRaised"], 0.45)
    popup = P["surfaceOverlay"] if light else m(0.035)
    if oled:
        popup = bg
    acc = P["accent"]
    acc_hi = mix(acc, (0, 0, 0), 0.15) if light else mix(acc, (255, 255, 255), 0.2)
    f = 0.06 if light else (0.09 if oled else 0.075)  # light fills need less lift to read as fields
    col = {
        "Text": fg, "TextDisabled": P["textMuted"],
        "WindowBg": bg, "ChildBg": (0, 0, 0, 0), "PopupBg": popup,
        "Border": P["border"], "BorderShadow": (0, 0, 0, 0),
        "FrameBg": m(f), "FrameBgHovered": m(f + 0.03), "FrameBgActive": m(f + 0.06),
        "TitleBg": P["chrome"], "TitleBgActive": P["chrome"], "TitleBgCollapsed": P["chrome"], "MenuBarBg": P["chrome"],
        "ScrollbarBg": (0, 0, 0, 0), "ScrollbarGrab": m(0.13), "ScrollbarGrabHovered": m(0.2), "ScrollbarGrabActive": m(0.27),
        "CheckMark": acc, "SliderGrab": acc, "SliderGrabActive": acc_hi,
        "Button": m(f + 0.03), "ButtonHovered": m(f + 0.06), "ButtonActive": m(f + 0.09),
        "Header": m(f + 0.07), "HeaderHovered": m(f + 0.035), "HeaderActive": m(f + 0.09),  # selected rows sit above the field fill
        "Separator": P["border"], "SeparatorHovered": alpha(acc, 0x80), "SeparatorActive": acc,
        "ResizeGrip": (0, 0, 0, 0), "ResizeGripHovered": alpha(acc, 0x80), "ResizeGripActive": acc,
        "Tab": (0, 0, 0, 0), "TabHovered": m(f * 0.8), "TabActive": m(f + 0.07) if oled else m(f + 0.015),
        "TabUnfocused": (0, 0, 0, 0), "TabUnfocusedActive": m(f),
        "PlotLines": acc, "PlotLinesHovered": acc_hi, "PlotHistogram": acc, "PlotHistogramHovered": acc_hi,
        "TextSelectedBg": alpha(acc, 0x55), "DragDropTarget": acc, "NavHighlight": acc,
        "NavWindowingHighlight": alpha(fg, 0xB3), "NavWindowingDimBg": (0, 0, 0, 0x66),
        "ModalWindowDimBg": (0, 0, 0, 0x40 if light else 0x99),
        "TableHeaderBg": m(0.035), "TableBorderStrong": (0, 0, 0, 0), "TableBorderLight": (0, 0, 0, 0),
        "TableRowBg": (0, 0, 0, 0), "TableRowBgAlt": (0, 0, 0, 0),
    }
    ex = {k: parse(v) for k, v in EXTRA["light" if light else "dark"].items()}
    sd = {
        "red": P["error"], "green": ex["green"], "blue": acc, "yellow": ex["yellow"], "orange": P["warning"],
        "cyan": ex["cyan"], "fuchsia": ex["fuchsia"], "magenta": ex["magenta"], "lavender": ex["lavender"],
        "light_green": ex["light_green"], "light_cyan": ex["light_cyan"], "constellation": acc_hi,
        "plot_bg": card, "fft_graduations": (0, 0, 0, 0x1F) if light else (255, 255, 255, 0x1C),
        "notification_bg": alpha(popup, 0xF2), "widget_bg": card, "frame_bg": popup,
        "overlay_bg": alpha(card, 0), "freq_highlight": alpha(acc, 0x40), "treeview_icon": P["textMuted"],
    }
    soft = {
        "enabled": True, "row_rounding": 8.0, "card_rounding": 14.0, "check_size": 18.0,
        "control_border": hx(m(0.25 if light else (0.28 if oled else 0.2))), "on_accent": hx(P["accentForeground"]),
        "card_bg": hx(card), "card_border": hx(P["border"]), "muted": hx(P["textMuted"]),
    }
    return {
        "light": light, "font": "Inter-Regular", "font_size": 16,
        "ImGuiStyle": {
            "WindowRounding": 14.0, "WindowBorderSize": 1.0, "ChildRounding": 12.0, "ChildBorderSize": 1.0,
            "FrameRounding": 10.0, "FrameBorderSize": 0.0, "PopupRounding": 12.0, "PopupBorderSize": 1.0,
            "GrabRounding": 10.0, "GrabMinSize": 12.0, "ScrollbarRounding": 10.0, "ScrollbarSize": 8.0,
            "TabRounding": 10.0, "TabBorderSize": 0.0, "TabBarBorderSize": 0.0, "SeparatorTextBorderSize": 0.0,
            "IndentSpacing": 18.0, "WindowPadding": [14, 12], "FramePadding": [12, 7], "ItemSpacing": [10, 8],
            "ItemInnerSpacing": [8, 6], "CellPadding": [8, 5], "SeparatorTextPadding": [4, 6],
        },
        "Soft": soft,
        "ImGuiColors": {k: hx(v) for k, v in col.items()},
        "SatDumpColors": {k: hx(v) for k, v in sd.items()},
    }


def make_all():
    with open(os.path.join(HERE, "palettes.json")) as fh:
        palettes = json.load(fh)
    made = []
    for key, c in palettes.items():
        base, mode = key.rsplit("-", 1)
        made.append((f"{NAMES[base]} {mode.capitalize()}", build(c, mode == "light")))
        if key == "soft-dark":
            made.append((OLED_NAME, build(c, False, oled=True)))
    return made


def write(out, made):
    os.makedirs(out, exist_ok=True)
    for name, theme in made:
        with open(os.path.join(out, name + ".json"), "w") as fh:
            json.dump(theme, fh, indent=4)
            fh.write("\n")


def lint(name, theme):
    """Return a list of problems with one generated theme."""
    problems = []
    for key, value in theme["ImGuiStyle"].items():
        if isinstance(value, list):
            if len(value) != 2 or not all(isinstance(v, (int, float)) for v in value):
                problems.append(f"{name}: ImGuiStyle.{key} must be [x, y]")
        elif not isinstance(value, float):
            problems.append(f"{name}: ImGuiStyle.{key} must be a float (SatDump ignores integers)")
    for group in ("ImGuiColors", "SatDumpColors", "Soft"):
        for key, value in theme[group].items():
            if isinstance(value, str) and not (len(value) == 9 and value[0] == "#" and all(ch in "0123456789ABCDEF" for ch in value[1:])):
                problems.append(f"{name}: {group}.{key} = {value!r} is not #RRGGBBAA")
    if name == OLED_NAME:
        for key in ("WindowBg", "TitleBg", "MenuBarBg"):
            if theme["ImGuiColors"][key] != "#000000FF":
                problems.append(f"{name}: {key} must be pure black")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir", help="theme folder, normally resources/themes")
    ap.add_argument("--check", action="store_true", help="compare the committed files with a fresh generation")
    args = ap.parse_args()
    made = make_all()
    problems = [p for name, theme in made for p in lint(name, theme)]
    if args.check:
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp, made)
            for name, _ in made:
                fresh = os.path.join(tmp, name + ".json")
                committed = os.path.join(args.outdir, name + ".json")
                if not os.path.exists(committed):
                    problems.append(f"{name}: missing from {args.outdir}")
                    continue
                with open(fresh, "rb") as a, open(committed, "rb") as b:
                    if a.read() != b.read():
                        problems.append(f"{name}: committed file differs from the generator output")
    else:
        write(args.outdir, made)
        for name, theme in made:
            print(name, theme["ImGuiColors"]["WindowBg"], theme["ImGuiColors"]["FrameBg"], theme["ImGuiColors"]["CheckMark"])
    for p in problems:
        print("theme_gen:", p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
