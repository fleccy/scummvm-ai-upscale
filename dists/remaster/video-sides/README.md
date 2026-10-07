# Widescreen cutscenes (optional)

COMI's cutscenes are 640x480. In widescreen the game can show them at full 16:9 width using side strips from
`remaster_video_sides_path` (one `<VIDEO>.sides` file per video), instead of black bars. The strips are made on
the player's PC from their own installation; `generate_cutscenes.cmd` / `generate_cutscenes.ps1` in the install
folder run the steps below with the paths set from the installed game.

## Pipeline

1. `batch_outpaint.py` decodes every `.SAN` video of the installed game with ffmpeg (it reads COMI's SMUSH format,
   frame `n` = the game's frame `n + 1`), splits it into shots at hard cuts, and widens each shot to 16:9 with the
   [AI Remaster Pipeline](https://github.com/dtaddis/ai-remaster-pipeline) LTX-2.3 outpainting (local, ComfyUI).
   Resumable; about 1 minute per short shot on an RTX 5080.
2. `pack_sides.py <out dir>` turns the results into one `<NAME>.sides` file per video: per frame, the left and
   right 104 px strips, colour-matched to the real video (ARP grades its whole output), feathered into the video's
   edge, and quantised to that frame's own palette so they follow the video's palette fades; zlib per frame.
3. `ocean_synth.py` (optional): for shots of open water, rebuilds the strips' water from the video's own water so
   it moves exactly like the real frame.

Both scripts read `COMI_GAME` (folder with the game's `.SAN` files), `COMI_ARP` (the ARP install) and
`COMI_VIDEO_WORK` (working folder) from the environment.

## In the game

When a video plays in widescreen and a file exists for it, each 640x480 frame is drawn with its strips as one
848x480 frame, so the AI upscaler and the subtitles work as usual. Frames without strips (or
`remaster_video_sides=off`) keep black bars; F12 switches live.

Dev tools: `remaster_video_frames_dir` saves every decoded frame; `video.request` in the dump dir
(`<n> <FILE.SAN>`) plays a cutscene directly.
