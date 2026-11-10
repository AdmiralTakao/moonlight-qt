# Moonlight GFN-Fork

Inoffizieller Fork von [Moonlight](https://github.com/moonlight-stream/moonlight-qt) mit Funktionen nach dem Vorbild von GeForce NOW.
Die neuen Optionen findest du in den Einstellungen unter **„GFN-Features (Fork)“**.

## Funktionen (Windows, Direct3D-11-Renderer)

| Funktion | Was sie macht |
|---|---|
| **Schnellprofile** | Competitive / Balanced / Qualität stellen V-Sync, Frame-Pacing, Upscaling, Schärfe und Low-Latency mit einem Klick ein. |
| **KI-Upscaling** | Rechnet den Stream auf die Fenstergröße hoch. Auf NVIDIA RTX mit *RTX Video Super Resolution*, auf Intel mit *Intel VSR*, auf allen anderen GPUs (z. B. AMD) mit **AMD FSR 1**. Greift nur, wenn die Stream-Auflösung kleiner ist als das Fenster. |
| **Bildschärfe** | AMD RCAS-Nachschärfung (0–100 %), mit Rauschunterdrückung. |
| **Künstliches 4:4:4** | Rekonstruiert die halbierte Farbauflösung (4:2:0) anhand der Helligkeit → klarere Schrift und Farbkanten. |
| **Low-Latency-Modus** | V-Sync/Frame-Pacing aus, nur das neueste Bild wird angezeigt, höhere CPU/GPU-Priorität. |

### NVIDIA RTX Video Super Resolution aktivieren
NVIDIA App → Grafik → *RTX Video Enhancement* → **Super Resolution** einschalten
(alternativ: NVIDIA Systemsteuerung → Videobildeinstellungen anpassen → RTX-Videoverbesserung).

### Frame Generation
Am besten auf dem Gaming-PC aktivieren (AMD Fluid Motion Frames, DLSS Frame Generation).
Am Client würde Frame Generation die Latenz erhöhen.

## Installer herunterladen
GitHub → Reiter **Actions** → neuester erfolgreicher Lauf von „Build“ → unten unter *Artifacts*
**Moonlight-GFN-Setup-…** herunterladen (ZIP mit `MoonlightSetup-….exe`).

## Technisches
- Shader: `app/shaders/d3d11_yuv420_cfl_pixel.hlsl`, `d3d11_fsr_easu_pixel.hlsl`, `d3d11_fsr_rcas_pixel.hlsl`
  (werden im CI von `app/shaders/build_gfn_hlsl.ps1` mit `fxc` kompiliert; FSR-Lizenz: `app/shaders/LICENSE-FSR`).
- Renderer: `app/streaming/video/ffmpeg-renderers/d3d11va.cpp` (Abschnitt „GFN fork“).
- Fallen Shader oder Treiber-Upscaling aus, nutzt Moonlight automatisch den normalen Renderer.
- Lizenz: GPLv3 wie das Original-Moonlight.
