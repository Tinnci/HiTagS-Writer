#!/usr/bin/env python3
"""
Flipper Zero UI Layout Analyzer for HiTagS Writer
===================================================
Simulates the 128x64 monochrome screen and checks for text/icon overlaps.

Uses Flipper's font metrics:
- FontPrimary: 10px height, ~6px char width (variable)
- FontSecondary: 8px height, ~5px char width (variable)
- Button bar: 13px at bottom (y=51..63)

Icons (approximate bounding boxes from Flipper assets):
- I_NFC_manual_60x50:        60x50 px
- I_RFIDDolphinSend_97x61:   97x61 px
- I_DolphinSuccess_91x55:    91x55 px
- I_WarningDolphinFlip_45x42: 45x42 px
"""

from dataclasses import dataclass, field
from typing import List, Tuple, Optional
import sys

# Screen dimensions
SCREEN_W = 128
SCREEN_H = 64

# Font metrics (approximate)
FONT_PRIMARY_H = 10
FONT_PRIMARY_CHAR_W = 6
FONT_SECONDARY_H = 8
FONT_SECONDARY_CHAR_W = 5

# Button bar
BUTTON_BAR_H = 13
BUTTON_BAR_Y = SCREEN_H - BUTTON_BAR_H  # y=51

# Icon sizes (w, h)
ICONS = {
    "I_NFC_manual_60x50":        (60, 50),
    "I_RFIDDolphinSend_97x61":   (97, 61),
    "I_DolphinSuccess_91x55":    (91, 55),
    "I_WarningDolphinFlip_45x42":(45, 42),
}


@dataclass
class Rect:
    """Axis-aligned bounding box"""
    x: int
    y: int
    w: int
    h: int
    label: str = ""

    @property
    def x2(self): return self.x + self.w
    @property
    def y2(self): return self.y + self.h

    def intersects(self, other: 'Rect') -> bool:
        return not (self.x2 <= other.x or other.x2 <= self.x or
                    self.y2 <= other.y or other.y2 <= self.y)

    def overlap_area(self, other: 'Rect') -> int:
        ox = max(0, min(self.x2, other.x2) - max(self.x, other.x))
        oy = max(0, min(self.y2, other.y2) - max(self.y, other.y))
        return ox * oy

    def out_of_bounds(self) -> bool:
        return self.x < 0 or self.y < 0 or self.x2 > SCREEN_W or self.y2 > SCREEN_H


def text_rect(x: int, y: int, text: str, font: str, align_h: str, align_v: str) -> List[Rect]:
    """Compute bounding rect(s) for text, handling \\n as multi-line.
    Returns one Rect per line."""
    if font == "FontPrimary":
        char_w, line_h = FONT_PRIMARY_CHAR_W, FONT_PRIMARY_H
    else:
        char_w, line_h = FONT_SECONDARY_CHAR_W, FONT_SECONDARY_H

    lines = text.split("\\n") if "\\n" in text else text.split("\n")
    rects = []

    cur_y = y
    for i, line in enumerate(lines):
        w = len(line) * char_w
        h = line_h

        # Horizontal alignment
        if align_h == "AlignCenter":
            rx = x - w // 2
        elif align_h == "AlignRight":
            rx = x - w
        else:  # AlignLeft
            rx = x

        # Vertical alignment (only for first line)
        if i == 0:
            if align_v == "AlignCenter":
                cur_y = y - h // 2
            elif align_v == "AlignBottom":
                cur_y = y - h * len(lines)

        rects.append(Rect(rx, cur_y, w, h, f'text:"{line}"'))
        cur_y += h

    return rects


def icon_rect(x: int, y: int, icon_name: str) -> Rect:
    w, h = ICONS.get(icon_name, (10, 10))
    return Rect(x, y, w, h, f"icon:{icon_name}")


def button_rect(btn_type: str, text: str) -> Rect:
    """Approximate button position in button bar"""
    char_w = FONT_SECONDARY_CHAR_W
    padding = 8
    w = len(text) * char_w + padding * 2

    if btn_type == "GuiButtonTypeLeft":
        return Rect(0, BUTTON_BAR_Y, w, BUTTON_BAR_H, f'btn:"{text}"')
    elif btn_type == "GuiButtonTypeRight":
        return Rect(SCREEN_W - w, BUTTON_BAR_Y, w, BUTTON_BAR_H, f'btn:"{text}"')
    else:  # Center
        return Rect((SCREEN_W - w) // 2, BUTTON_BAR_Y, w, BUTTON_BAR_H, f'btn:"{text}"')


@dataclass
class Screen:
    """A screen state (scene + view state)"""
    name: str
    elements: List[Rect] = field(default_factory=list)

    def add_icon(self, x, y, icon_name):
        self.elements.append(icon_rect(x, y, icon_name))

    def add_text(self, x, y, text, font="FontSecondary", align_h="AlignCenter", align_v="AlignTop"):
        self.elements.extend(text_rect(x, y, text, font, align_h, align_v))

    def add_button(self, btn_type, text):
        self.elements.append(button_rect(btn_type, text))

    def check(self):
        """Check for overlaps and out-of-bounds"""
        issues = []

        # Out of bounds check
        for e in self.elements:
            if e.out_of_bounds():
                issues.append(f"  OOB: {e.label} @ ({e.x},{e.y})-({e.x2},{e.y2})")

        # Text-icon overlap check (skip icon-icon since they're intentional backgrounds)
        text_elems = [e for e in self.elements if e.label.startswith("text:")]
        icon_elems = [e for e in self.elements if e.label.startswith("icon:")]
        btn_elems = [e for e in self.elements if e.label.startswith("btn:")]

        # Text-text overlap
        for i in range(len(text_elems)):
            for j in range(i+1, len(text_elems)):
                a, b = text_elems[i], text_elems[j]
                area = a.overlap_area(b)
                if area > 0:
                    issues.append(f"  TEXT OVERLAP ({area}px²): {a.label} vs {b.label}")

        # Text clipped by icon (optional - icons are usually backgrounds)
        # Just note when text lands on top of an icon
        for t in text_elems:
            for ic in icon_elems:
                if t.intersects(ic):
                    area = t.overlap_area(ic)
                    # Only warn if significant overlap
                    if area > t.w * t.h * 0.3:
                        issues.append(f"  NOTE: text over icon: {t.label} ∩ {ic.label} ({area}px²)")

        # Button-text overlap
        for b in btn_elems:
            for t in text_elems:
                if b.intersects(t):
                    area = b.overlap_area(t)
                    if area > 0:
                        issues.append(f"  BTN-TEXT OVERLAP ({area}px²): {b.label} vs {t.label}")

        return issues

    def render_ascii(self, width=64, height=32):
        """Render a scaled ASCII preview of the screen"""
        sx = SCREEN_W / width
        sy = SCREEN_H / height
        grid = [['.' for _ in range(width)] for _ in range(height)]

        chars = {'icon': '#', 'text': 'T', 'btn': 'B'}

        for e in self.elements:
            prefix = e.label.split(":")[0]
            ch = chars.get(prefix, '?')
            for py in range(height):
                for px in range(width):
                    rx = int(px * sx)
                    ry = int(py * sy)
                    if e.x <= rx < e.x2 and e.y <= ry < e.y2:
                        if grid[py][px] == '.':
                            grid[py][px] = ch
                        elif grid[py][px] != ch:
                            grid[py][px] = 'X'  # Overlap!

        lines = [''.join(row) for row in grid]
        return '\n'.join(lines)


def build_all_screens() -> List[Screen]:
    """Build all screen states from our scene analysis"""
    screens = []

    # ========== Reading Popups (common pattern) ==========
    for name in ["ReadUID_Scanning", "ReadTag_Scanning", "FullDump_Scanning"]:
        s = Screen(name)
        s.add_icon(0, 3, "I_NFC_manual_60x50")
        s.add_text(89, 30, "Reading...", "FontPrimary", "AlignCenter", "AlignTop")
        s.add_text(89, 43, "Place tag on\nFlipper back", "FontSecondary", "AlignCenter", "AlignTop")
        screens.append(s)

    # Note: FullDump uses "Dumping..." header instead of "Reading..."
    screens[-1].elements[1] = text_rect(89, 30, "Dumping...", "FontPrimary", "AlignCenter", "AlignTop")[0]

    # ========== Writing Popups (common pattern) ==========
    for name, header, body in [
        ("Write_Writing", "Writing...", "AA:BB:CC:DD:EE"),
        ("WriteUID_Writing", "Writing...", "AABBCCDD"),
        ("Clone_Cloning", "Cloning...", "UID: AABBCCDD\n60 pages"),
        ("Wipe_Wiping", "Wiping Tag...", "Place tag on\nFlipper back"),
    ]:
        s = Screen(name)
        s.add_icon(0, 3, "I_RFIDDolphinSend_97x61")
        s.add_text(89, 30, header, "FontPrimary", "AlignCenter", "AlignTop")
        s.add_text(89, 43, body, "FontSecondary", "AlignCenter", "AlignTop")
        screens.append(s)

    # ========== Success Popups (common pattern) ==========
    for name, header, body in [
        ("WriteSuccess", "Success!", "ID written\nAA:BB:CC:DD:EE"),
        ("WriteUID_Success", "UID Done!", "New UID:\nAABBCCDD"),
        ("Clone_Success", "Cloned!", "UID: AABBCCDD\n60 pgs cloned"),
        ("Wipe_Success", "Tag Wiped!", "60 pgs wiped\nConfig reset"),
        ("FullDump_Saved", "Saved!", "AABBCCDD.hts"),
    ]:
        s = Screen(name)
        s.add_icon(0, 9, "I_DolphinSuccess_91x55")
        s.add_text(97, 12, header, "FontPrimary", "AlignCenter", "AlignTop")
        s.add_text(97, 25, body, "FontSecondary", "AlignCenter", "AlignTop")
        screens.append(s)

    # ========== Success Widgets (with buttons) ==========
    # ReadUID Success
    s = Screen("ReadUID_Success")
    s.add_icon(0, 9, "I_DolphinSuccess_91x55")
    s.add_text(97, 2, "Tag Found!", "FontPrimary", "AlignCenter", "AlignTop")
    s.add_text(97, 16, "UID: AABBCCDD\nAA:BB:CC:DD", "FontSecondary", "AlignCenter", "AlignTop")
    s.add_button("GuiButtonTypeCenter", "OK")
    screens.append(s)

    # ReadTag Success
    s = Screen("ReadTag_Success")
    s.add_icon(0, 9, "I_DolphinSuccess_91x55")
    s.add_text(97, 0, "EM4100 ID", "FontPrimary", "AlignCenter", "AlignTop")
    s.add_text(97, 14, "AA:BB:CC:DD:EE\nAABBCCDD", "FontSecondary", "AlignCenter", "AlignTop")
    s.add_button("GuiButtonTypeCenter", "OK")
    screens.append(s)

    # FullDump Success
    s = Screen("FullDump_Success")
    s.add_text(64, 0, "Tag Dump", "FontPrimary", "AlignCenter", "AlignTop")
    s.add_text(64, 13, "UID:AABBCCDD MEMT:3\n60/63 pgs auth:1", "FontSecondary", "AlignCenter", "AlignTop")
    s.add_button("GuiButtonTypeLeft", "Back")
    s.add_button("GuiButtonTypeCenter", "Save")
    s.add_button("GuiButtonTypeRight", "Log")
    screens.append(s)

    # ========== Failure Widgets (common pattern) ==========
    err_short = "No tag found.\nPlace tag on\nFlipper back."
    err_auth = "Auth rejected.\nWrong password\nor not 8268?"

    for name, title, errmsg in [
        ("ReadUID_Fail", "Read Failed!", err_short),
        ("ReadTag_Fail", "Read Failed!", err_short),
        ("WriteFail", "Write Failed!", err_short),
        ("WriteUID_Fail", "Write Failed!", err_auth),
        ("Clone_Fail", "Clone Failed!", err_auth),
        ("Wipe_Fail", "Wipe Failed!", err_short),
        ("FullDump_Fail", "Dump Failed!", err_short),
    ]:
        s = Screen(name)
        s.add_icon(83, 22, "I_WarningDolphinFlip_45x42")
        s.add_text(40, 5, title, "FontPrimary", "AlignCenter", "AlignTop")
        s.add_text(40, 22, errmsg, "FontSecondary", "AlignCenter", "AlignTop")
        s.add_button("GuiButtonTypeLeft", "Back")
        s.add_button("GuiButtonTypeRight", "Retry")
        screens.append(s)

    # ========== Confirm Dialogs ==========
    # WriteConfirm
    s = Screen("WriteConfirm")
    s.add_icon(0, 12, "I_NFC_manual_60x50")
    s.add_text(64, 0, "Confirm Write", "FontPrimary", "AlignCenter", "AlignTop")
    s.add_text(64, 16, "Write EM4100\nAA:BB:CC:DD:EE\nto 8268 tag?", "FontSecondary", "AlignCenter", "AlignTop")
    screens.append(s)

    # WriteUID Confirm
    s = Screen("WriteUID_Confirm")
    s.add_text(64, 0, "Write UID", "FontPrimary", "AlignCenter", "AlignTop")
    s.add_text(64, 16, "Write UID\nAABBCCDD\nto 8268 page 0?", "FontSecondary", "AlignCenter", "AlignTop")
    screens.append(s)

    # Wipe Confirm
    s = Screen("Wipe_Confirm")
    s.add_text(64, 0, "Wipe 8268 Tag?", "FontPrimary", "AlignCenter", "AlignTop")
    s.add_text(64, 16, "This will ERASE all data\nand reset config+PWD\nto factory defaults.",
               "FontSecondary", "AlignCenter", "AlignTop")
    screens.append(s)

    # Clone Confirm
    s = Screen("Clone_Confirm")
    s.add_text(64, 0, "Clone from Dump", "FontPrimary", "AlignCenter", "AlignTop")
    s.add_text(64, 14, "UID: AABBCCDD\nMEMT:3 60 data pgs\nauth:1 TTFM:0",
               "FontSecondary", "AlignCenter", "AlignTop")
    screens.append(s)

    # ========== About ==========
    s = Screen("About")
    s.add_text(64, 2, "HiTagS Writer v0.3", "FontPrimary", "AlignCenter", "AlignTop")
    s.add_text(0, 16, "Write EM4100 card data to\nHiTag S 8268 magic chips.\n\nSupported chips:\n8268/F8268/F8278/K8678\nDefault PWD: BBDD3399",
               "FontSecondary", "AlignLeft", "AlignTop")
    screens.append(s)

    return screens


def main():
    screens = build_all_screens()
    total_issues = 0
    problem_screens = []

    print("=" * 70)
    print("HiTagS Writer — UI Layout Analysis")
    print(f"Screen: {SCREEN_W}x{SCREEN_H} px")
    print("=" * 70)

    for scr in screens:
        issues = scr.check()
        if issues:
            total_issues += len(issues)
            problem_screens.append(scr.name)
            print(f"\n{'!'*3} {scr.name} — {len(issues)} issue(s):")
            for iss in issues:
                print(iss)
            print(f"\n  ASCII preview:")
            print("  +" + "-" * 64 + "+")
            for line in scr.render_ascii().split("\n"):
                print(f"  |{line}|")
            print("  +" + "-" * 64 + "+")
        else:
            print(f"  OK: {scr.name}")

    print("\n" + "=" * 70)
    print(f"Summary: {len(screens)} screens checked, {total_issues} issues in {len(problem_screens)} screens")
    if problem_screens:
        print(f"Problem screens: {', '.join(problem_screens)}")
    else:
        print("All layouts look clean!")

    # Detailed layout dump for any screen with issues
    if "--detail" in sys.argv:
        print("\n\n=== DETAILED ELEMENT DUMP ===")
        for scr in screens:
            print(f"\n--- {scr.name} ---")
            for e in scr.elements:
                oob = " [OOB!]" if e.out_of_bounds() else ""
                print(f"  ({e.x:3d},{e.y:2d})-({e.x2:3d},{e.y2:2d}) {e.w:3d}x{e.h:2d} {e.label}{oob}")

    return 1 if total_issues > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
