# Moonlight GFN-Fork

Inoffizieller Fork von [Moonlight](https://github.com/moonlight-stream/moonlight-qt) mit Funktionen nach dem Vorbild von GeForce NOW.
Die neuen Optionen findest du in den Einstellungen unter **„GFN-Features (Fork)“**.

## Funktionen (Windows, Direct3D-11-Renderer)

| Funktion | Was sie macht |
|---|---|
| **Schnellprofile** | Competitive / Balanced / Qualität stellen V-Sync, Frame-Pacing, Upscaling, Schärfe und Low-Latency mit einem Klick ein. |
| **KI-Upscaling** | Rechnet den Stream auf die Fenstergröße hoch. Auf NVIDIA RTX mit *RTX Video Super Resolution*, auf Intel mit *Intel VSR*, auf allen anderen GPUs (z. B. AMD) mit **MWSR**. Greift, wenn die Stream-Auflösung kleiner ist als das Fenster. |
| **MWSR** | *MoonlightWeb Super Resolution* von linckosz (MIT): ein einziger Durchgang, auf integrierten Grafikchips mehrfach schneller als FSR 1 bei ähnlicher oder besserer Qualität. Verkleinert auch sauber, wenn der Stream größer ist als das Fenster. HDR/10-Bit nutzt weiter FSR. |
| **AMD FSR 1** | Klassisches FSR 1 (EASU + RCAS), läuft auf allen GPUs. |
| **Bildschärfe** | AMD RCAS-Nachschärfung (0–100 %), mit Rauschunterdrückung. |
| **Künstliches 4:4:4** | Rekonstruiert die halbierte Farbauflösung (4:2:0) anhand der Helligkeit → klarere Schrift und Farbkanten. |
| **Low-Latency-Modus** | V-Sync/Frame-Pacing aus, nur das neueste Bild wird angezeigt, höhere CPU/GPU-Priorität. |
| **Statusanzeige** | Das Statistik-Overlay (Strg+Alt+Umschalt+S) zeigt in der Zeile *Bildverbesserung*, ob und wie gerade hochgerechnet wird – und wenn nicht, warum (z. B. „Stream = Fenster“ oder ein anderer Renderer). |

> Gegen Blockartefakte („pixelig“) hilft kein Upscaler – nur mehr Bitrate und HEVC statt H.264.

## Start mit einem Tipp (alle Plattformen)
Rechtsklick auf einen PC → **„Weck-Adresse festlegen…“**. Ist eine Weck-Adresse gesetzt, reicht ein Klick auf den
ausgeschalteten PC: Moonlight ruft die Adresse auf, schickt zusätzlich Wake-on-LAN (falls die MAC-Adresse bekannt ist),
wartet bis zu 3 Minuten, bis der PC online ist, und öffnet dann die App-Liste.

Beispiele:
- Shelly-Steckdose: `http://192.168.178.50/relay/0?turn=on`
- Tasmota-Steckdose: `http://192.168.178.51/cm?cmnd=Power%20On`
- UpSnap / Home Assistant: deren Weck-URL; für Webhooks, die POST brauchen, `POST ` voranstellen
- Zugangsdaten gehen über `http://benutzer:passwort@adresse/...`

Die Adresse muss von dort erreichbar sein, wo du gerade bist – unterwegs z. B. über einen Tailscale-Subnet-Router
(NAS, Raspberry Pi). Steckdose: im BIOS „Restore on AC Power Loss: Power On“ einstellen und den PC vor dem
Ausschalten der Steckdose immer herunterfahren.

Ohne Weck-Adresse gibt es im Menü **„Wecken und verbinden“** (nur Wake-on-LAN, wartet ebenfalls und verbindet).

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
- Shader: `app/shaders/d3d11_yuv420_cfl_pixel.hlsl`, `d3d11_fsr_easu_pixel.hlsl`, `d3d11_fsr_rcas_pixel.hlsl`,
  `app/shaders/mwsr/*.hlsl` (werden im CI von `app/shaders/build_gfn_hlsl.ps1` mit `fxc` kompiliert;
  Lizenzen: `app/shaders/LICENSE-FSR`, `app/shaders/mwsr/LICENSE-MWSR`).
- MWSR-Konstanten: `app/streaming/video/ffmpeg-renderers/mwsr.h` (unverändert aus linckosz/moonlight-web).
- Weck-Adressen: `app/gui/computermodel.cpp` (gespeichert in den Einstellungen unter `gfn_wakeurls/<PC-UUID>`).
- Renderer: `app/streaming/video/ffmpeg-renderers/d3d11va.cpp` (Abschnitt „GFN fork“).
- Fallen Shader oder Treiber-Upscaling aus, nutzt Moonlight automatisch den normalen Renderer.
- Lizenz: GPLv3 wie das Original-Moonlight.
