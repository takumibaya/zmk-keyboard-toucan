# ZMK config for beekeeb Toucan Keyboard

[The beekeeb Toucan Keyboard](https://beekeeb.com/toucan-keyboard/) is a wireless split 42-key column‑stagger keyboard that a display and a trackpad, with an aggressive stagger on the pinky columns.

## Trackpad gestures

Tap-to-click checks the peak `|x - x_down| + |y - y_down|` over the touch, in raw
sensor counts before acceleration. The defaults allow up to 44 counts and 170 ms,
including the right half's release delay. Small back-and-forth jitter does not
accumulate, but moving past the limit and returning does not restore a tap.

Rim scroll waits for that movement limit to be exceeded. The next coordinate
must be in the calibrated rim band, and the net angle must then reach the
configured start angle before cursor motion switches to scrolling.

# License

The code in this repo is available under the MIT license.

The included shield nice_view_gem is modified from https://github.com/M165437/nice-view-gem licensed under the MIT License.

ZMK code snippets are taken from the ZMK documentation under the MIT license.

The embedded font QuinqueFive is designed by GGBotNet, licensed under under the SIL Open Font License, Version 1.1.
