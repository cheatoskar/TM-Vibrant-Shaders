# 🏎️ TM Vibrant Shaders

> **Minecraft-inspiriertes Shaderpack & TMModloader-Plugin für TrackMania Nations Forever & United Forever**  
> Bringt den visuellen Look von **Sildur's Vibrant Shaders**, **BSL Shaders** und **IterationT** direkt auf die Rennstrecke!

[![Platform](https://img.shields.io/badge/Platform-TrackMania%20Forever-blue.svg)](https://tomashu.dev/software/tmloader/)
[![ModLoader](https://img.shields.io/badge/TMModloader-Supported-brightgreen.svg)](https://tomashu.dev/software/tmloader/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## ✨ Was ist TM Vibrant Shaders?

In Minecraft gehören Shader wie **Sildur's Vibrant**, **BSL** und **IterationT** zu den beliebtesten Grafik-Upgrades: warmes Sonnenlicht, spektakuläre volumetrische Lichtstrahlen (God Rays), weicher Bloom, anamorphotische Lens-Flare-Streifen und stimmungsvoller Horizont-Dunst.

**TM Vibrant Shaders** bringt genau diese Ästhetik nach **TrackMania Nations Forever (TMNF)** und **TrackMania United Forever (TMUF)**:

- ☀️ **Volumetrische God Rays / Sun Rays (`TM_SunRays.fx`):**  
  Sonnenstrahlen, die durch Stadionüberdachungen, Loopings und Bäume brechen – mit Tiefenpuffer-Verdeckung.
- 🎨 **Filmisches Color Grading & ACES Tonemapping (`TM_VibrantColor.fx`):**  
  Satte Stadion-Grünflächen, strahlend blaue Himmel und warmes Sonnenlicht (Sildurs & BSL-Farbprofil) ohne Ausbrennen von hellen Bereichen.
- ✨ **Cinematic Bloom & Anamorphic Flares (`TM_CinematicBloom.fx`):**  
  Weicher Glow um Lichtquellen und die ikonischen horizontalen Flare-Streifen von *IterationT* bei Stadionflutlichtern und Rücklichtern.
- 🌫️ **Atmosphärischer Tiefennebel (`TM_AtmosphericFog.fx`):**  
  Volumetrischer Distanzdunst und Sonnenstreuung für realistische Weitsicht wie in Minecraft.
- 🏁 **Contact Shading & Micro-AO (`TM_DepthShading.fx`):**  
  Tiefenkontrast und Kontaktschatten an Streckenblöcken, Kurven und am Fahrzeugchassis.

---

## 🎮 Presets / Profile

| Preset | Stil | Merkmale |
|---|---|---|
| **Sildur's Vibrant** | 🌞 Warm, intensiv & lebendig | Kräftiges Sonnenlicht, leuchtendes Gras, cyanblauer Himmel, intensive God Rays. |
| **BSL Clean** | 🌿 Modern, filmisch & balanciert | ACES Tonemapping, dezente Strahlen, stimmungsvoller Distanzdunst, tiefer Schattenkontrast. |
| **IterationT** | 🎬 Filmreif & spektakulär | Horizontale anamorphe Lens Flares, intensiver weicher Bloom, hoher Kontrast. |
| **Custom** | 🛠️ Eigene Anpassung | Alle Parameter live im In-Game-Menü über Regler einstellbar. |

---

## 🚀 Schnelle Installation (TMModloader)

Genau wie beim [100% TMX + Bingo Plugin](https://github.com/cheatoskar/100-TMX-Bingo-Plugin) ist dieses Projekt direkt für den **TrackMania ModLoader** strukturiert!

### 1. Automatische Installation mit PowerShell
Führe im Projektordner einfach folgenden Befehl aus:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-modloader.ps1
```

Das Skript richtet automatisch die Mod-Struktur in deinem ModLoader-Verzeichnis ein:
`%LOCALAPPDATA%\TMLoader\database\TmForever\products\TM Vibrant Shaders\1.0.0\`

### 2. Im ModLoader aktivieren
1. Öffne den **TrackMania ModLoader (TMLoader)**.
2. Setze in deiner Profil-Liste ein Häkchen bei **TM Vibrant Shaders**.
3. Starte TrackMania Forever!

### 3. Tastenbelegung im Spiel
- **`[F8]`**: In-Game-Shader-Menü öffnen/schließen (Presets wählen, Regler verschieben).
- **`[F7]`**: Shader schnell ein-/ausschalten (A/B-Vergleich).

---

## 📁 Projektstruktur

```
TrackMania-Vibrant-Shaders/
├── CMakeLists.txt                # CMake Build-Konfiguration (Win32 / D3D9)
├── description.yaml              # Root TMModloader Produkt-Metadaten
├── install-modloader.ps1         # 1-Klick Installer für den TrackMania ModLoader
├── LICENSE                       # MIT Lizenz
├── README.md                     # Projektdokumentation
├── src/                          # C++ ModLoader Plugin Quellcode
│   ├── dllmain.cpp               # Mod-Einstiegspunkt & D3D9 Boot-Thread
│   ├── hook.h / hook.cpp         # Direct3D 9 VTable & IAT Hooking
│   ├── overlay.h / overlay.cpp   # In-Game ImGui Menü (F8)
│   ├── config.h / config.cpp     # Preset-Verwaltung & Konfiguration
│   └── version.h.in              # Versionsvorlage
├── Shaders/                      # ReShade FX Shaders
│   ├── ReShade.fxh               # D3D9 / ReShade Header
│   ├── TM_VibrantColor.fx        # Sildurs / BSL Farbgrading & ACES
│   ├── TM_SunRays.fx             # Volumetrische God Rays
│   ├── TM_CinematicBloom.fx      # Bloom & IterationT Anamorphic Flares
│   ├── TM_AtmosphericFog.fx      # Tiefenbasierter Distanznebel
│   └── TM_DepthShading.fx        # Micro-AO & Streckenkontrast
├── Presets/                      # Vorgefertigte Presets
│   ├── TM_Sildurs_Vibrant.ini
│   ├── TM_BSL_Clean.ini
│   └── TM_IterationT_Cinematic.ini
└── Docs/
    ├── INSTALL_DE.md             # Ausführliche deutsche Anleitung
    └── INSTALL_EN.md             # English Setup Guide
```

---

## 🔨 Selbst kompilieren

Voraussetzungen:
- Windows mit Visual Studio 2022 (C++ Build Tools)
- CMake 3.21+

```powershell
# 1. Solution für 32-Bit (Win32) erzeugen:
cmake -B build -A Win32

# 2. Release-DLL kompilieren:
cmake --build build --config Release

# 3. Direkt in den TMModloader installieren:
powershell -ExecutionPolicy Bypass -File .\install-modloader.ps1
```

---

## 💡 Grafik-Tipp für TrackMania Forever
Für die volle Tiefenpuffer-Funktionalität (Tiefennebel & God-Ray-Verdeckung):
- Im TrackMania Launcher unter **Konfigurieren -> Erweitert**:
- **Antialiasing (MSAA)** im Launcher deaktivieren oder auf gering stellen, da DirectX 9 bei manchen MSAA-Treibern den Depth Buffer für Post-Processing sperrt. (Post-Processing AA wie FXAA/SMAA funktioniert immer).

---

## 📜 Lizenz & Credits
- **Autor:** cheatoskar
- **Inspiriert von:** Sildur's Vibrant Shaders, BSL Shaders (Capt Tatsu), IterationT (Motschen)
- **Lizenz:** MIT License
