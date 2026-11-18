# Widescreen side art (optional)

Most COMI rooms are 640 pixels wide. In widescreen mode (`remaster_widescreen=wide`) the engine draws those rooms
centred in an 848-wide screen and can fill the 104 columns on each side with **side art** from
`remaster_sides_path` (one PNG per room); rooms without a file get the ambient glow or black bars (F12).

## Format

`NNNN.png` (room number, e.g. `0009.png`), 848 pixels wide and as tall as the room. The engine always draws the
game's own room in the middle 640 columns itself, so a side-art file only has to provide what lies outside the
original frame: columns 0-103 and 744-847. The 24 columns just inside each join are blended with the room's edge,
so continue the scene across them. Anything else in the middle is ignored. Side art can be made in any way:
painted by hand, drawn over, or generated.

## Generating it from your own copy

`generate.py` (run by `generate_sides.cmd` / `generate_sides.ps1` in the install folder, which set up its
Python environment) makes a full set from the player's own installation:

1. **extract**: the game itself saves each narrow room's background from the installed copy
   (`remaster_extract_dir`, one short minimised run per room) to `sides-work\room_NNNN.png`;
2. **paint**: an AI model of the player's choice continues the background outwards to 16:9: Google Gemini through
   the player's own OpenRouter key, or Stable Diffusion XL inpainting locally on an NVIDIA GPU;
3. **compose** (`compose.py`): fits the painted picture to the room, aligns the new parts at the joins with
   optical flow, cuts the join along the least visible seam and blends across it, then writes `sides\NNNN.png`.

It is resumable (rooms already done are kept) and only ever reads the installed game.

## Sharing a set

`strip_middle.py <in_dir> <out_dir>` blanks the middle of each image, keeping only the painted strips and the
blend bands, so a shared set contains only its author's painting.
