# ZMK config for beekeeb Toucan Keyboard

[The beekeeb Toucan Keyboard](https://beekeeb.com/toucan-keyboard/) is a wireless split 42-key column‑stagger keyboard that a display and a trackpad, with an aggressive stagger on the pinky columns.

## Trackpad gestures

Tap-to-click checks the peak `|x - x_down| + |y - y_down|` over the touch, in raw
sensor counts before acceleration. The defaults allow up to 44 counts and 170 ms,
including the right half's release delay. Small back-and-forth jitter does not
accumulate, but moving past the limit and returning does not restore a tap.

Rim scroll waits for that movement limit to be exceeded. The next coordinate
must be in the calibrated rim band. Scrolling requires a tangential arc with
at least 35° of net rotation, even if Studio requests a smaller angle.
Settings record version 2 ignores version 1, so the new defaults take effect
after flashing; both gestures start disabled until enabled again.

# License

The code in this repo is available under the MIT license.

The included shield nice_view_gem is modified from https://github.com/M165437/nice-view-gem licensed under the MIT License.

ZMK code snippets are taken from the ZMK documentation under the MIT license.

The embedded font QuinqueFive is designed by GGBotNet, licensed under under the SIL Open Font License, Version 1.1.
