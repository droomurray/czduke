#!/usr/bin/env python3
"""Assertions over the framebuffer dumps produced by smoke.sh."""
import sys

work = sys.argv[1]
W, H = 320, 170


def load(name):
    data = open(f"{work}/{name}.fb", "rb").read()
    assert len(data) == W * H * 2, f"{name}: unexpected size {len(data)}"
    return data


def differing(a, b):
    return sum(1 for i in range(0, len(a), 2) if a[i:i + 2] != b[i:i + 2])


def colours(data):
    return len({data[i:i + 2] for i in range(0, len(data), 2)})


title, menu, down, idle, turn, fire, a43 = (
    load(n) for n in ("title", "menu", "menu-down", "level-idle", "level-turn", "level-fire", "aspect43"))

assert colours(title) > 50, "title screen looks blank"
assert colours(menu) > 50, "main menu looks blank"
assert colours(idle) > 50, "level looks blank"
# Only the 3D view (above the status bar) is compared.
view = W * 2 * 130
n_turn = differing(idle[:view], turn[:view])
n_fire = differing(idle[:view], fire[:view])
assert n_turn > 5000, f"holding D did not turn the view ({n_turn} px differ)"
assert n_fire > 50, f"holding K did not fire ({n_fire} px differ)"
# The ammo counter sits in the status bar; firing must change that too.
assert idle[view:] != fire[view:], "firing did not use ammunition"
n_menu = differing(menu, down)
assert 20 < n_menu < 5000, f"S did not move the menu selection ({n_menu} px differ)"

# 4:3 mode: 226 px wide picture centred, so 47 black columns on each side.
for y in (5, 85, 165):
    row = a43[y * W * 2:(y + 1) * W * 2]
    assert row[:46 * 2] == bytes(46 * 2), "left pillarbox is not black"
    assert row[-46 * 2:] == bytes(46 * 2), "right pillarbox is not black"
    assert row[60 * 2:260 * 2] != bytes(200 * 2), "4:3 picture is missing"

print(f"title colours={colours(title)} level colours={colours(idle)} turn diff={n_turn}px "
      f"fire diff={n_fire}px menu diff={n_menu}px")
