# Installationsanleitung für TrackMania ModLoader & ReShade

Dieses Dokument beschreibt die Schritte, um **TM Vibrant Shaders** in TrackMania Nations Forever oder United Forever zu nutzen.

## Methode 1: Über TMModloader (Empfohlen)

Das Projekt ist exakt nach dem Standard des TrackMania ModLoaders aufgebaut (wie z.B. das *100% TMX + Bingo Plugin* oder der *Competition Patch*).

1. Öffne die PowerShell im Projektordner:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install-modloader.ps1
   ```
2. Das Skript kopiert alle Dateien automatisch in:
   `%LOCALAPPDATA%\TMLoader\database\TmForever\products\TM Vibrant Shaders\1.0.0\`
3. Starte den **TrackMania ModLoader**.
4. Setze in deiner Profil-Ansicht ein Häkchen bei **TM Vibrant Shaders**.
5. Klicke auf **Spielen** / Starten.
6. Drücke im Spiel **`[F8]`**, um das Shader-Menü aufzurufen, oder **`[F7]`** zum schnellen Umschalten.

---

## Methode 2: Eigenständige Nutzung mit ReShade

Falls du die Shader ohne TMModloader direkt mit ReShade nutzen möchtest:

1. Lade [ReShade](https://reshade.me/) herunter und wähle `TmForever.exe` (DirectX 9).
2. Kopiere die Dateien aus dem Ordner `Shaders/` in deinen TrackMania `reshade-shaders/Shaders/`-Ordner.
3. Kopiere die `.ini`-Dateien aus `Presets/` in dein TrackMania-Hauptverzeichnis.
4. Öffne im Spiel das ReShade-Menü (standardmäßig `Pos1` / `Home`) und wähle eines der Presets aus:
   - `TM_Sildurs_Vibrant.ini`
   - `TM_BSL_Clean.ini`
   - `TM_IterationT_Cinematic.ini`

---

## Fehlerbehebung: Tiefenpuffer (Depth Buffer)

Sollten Tiefeneffekte (Atmosphärischer Nebel, Verdeckung der Lichtstrahlen) nicht greifen:
1. Öffne den **TmForeverLauncher.exe**.
2. Gehe auf **Konfigurieren** -> **Erweitert**.
3. Deaktiviere **Antialiasing (MSAA)** im Launcher (auf *Aus* oder *None* stellen).
4. Hintergrund: In DirectX 9 blockieren manche Grafikkartentreiber den Zugriff auf den Tiefenpuffer, wenn Hardware-MSAA aktiv ist.
