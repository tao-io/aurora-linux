# Vendored M3/G15 instruction encoder

These files are copied from Dougall Johnson and TellowKrinkle's public
[`applegpu`](https://github.com/TellowKrinkle/applegpu) project at commit
`5f7dc046e2c6ce45304f365bee04b3bdca7260d3`.

The original BSD-3-Clause license is preserved in `LICENSE`. Our captured G16
compute corpus is instruction-compatible with this encoder for the subset used
by `g16mandelbrot.py`. The G16 executable stop is two bytes (`0e 00`), whereas
the encoder pads it to four bytes; the wrapper removes only that padding.
