# MWSR (MoonlightWeb Super Resolution)

Spatial upscaler/downscaler by Bruno Martin (linckosz), MIT License (see `LICENSE-MWSR`).

Source: https://github.com/linckosz/moonlight-web/tree/main/mwsr
(commit 19602688b4d5611dc4006166675cf526f1fab6a9). The `.hlsl` files here are unmodified copies;
`app/streaming/video/ffmpeg-renderers/mwsr.h` is the matching CPU-side header.

The GFN fork compiles only `MwsrPS` from these files (with `MWSR_CB_REGISTER=b1`, because `b0`
holds Moonlight's colour conversion constants) and draws it with Moonlight's shared vertex shader.
The variants are built by `app/shaders/build_gfn_hlsl.ps1`:

| Output | Source | Defines |
|---|---|---|
| `d3d11_mwsr_up_pixel.fxc` | `mwsr-up.hlsl` | – |
| `d3d11_mwsr_down4_pixel.fxc` | `mwsr-down-4.hlsl` | `MWSR_R_DOWN=1.0`, `MWSR_LINEAR_LIGHT=1` |
| `d3d11_mwsr_down9_pixel.fxc` | `mwsr-down-9.hlsl` | `MWSR_R_DOWN=1.25`, `MWSR_LINEAR_LIGHT=1` |
| `d3d11_mwsr_down16_pixel.fxc` | `mwsr-down-16.hlsl` | `MWSR_R_DOWN=1.25`, `MWSR_LINEAR_LIGHT=1` |
