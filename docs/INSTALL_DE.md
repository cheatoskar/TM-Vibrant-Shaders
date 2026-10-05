# TM Vibrant Shaders – Installation (Deutsch)

> **Beta:** Die Presets werden noch verfeinert, der Look kann sich mit neuen Versionen ändern.

## Voraussetzungen
- TrackMania Nations Forever oder United Forever, Windows 10/11
- Optional: [TrackMania ModLoader (TMLoader)](https://tomashu.dev/software/tmloader/). Es geht auch ohne.

## Installation mit dem Setup (empfohlen)
1. **`TM-Vibrant-Shaders.zip`** von der [Releases-Seite](https://github.com/cheatoskar/tmnf-vibrant-shaders/releases) herunterladen und entpacken.
2. TrackMania schließen, **`TM-Vibrant-Shaders-Setup.exe`** starten und auswählen:
   - **Für den ModLoader installieren:** Danach im ModLoader bei *TM Vibrant Shaders* einen Haken setzen und das Spiel starten.
   - **In den Spielordner installieren (ohne ModLoader):** Den TrackMania-Ordner mit der `TmForever.exe` auswählen und das Spiel ganz normal starten. Liegt das Spiel unter *Programme*, fragt Windows nach Admin-Rechten.

Wenn Windows SmartScreen warnt, liegt das nur daran, dass das Setup nicht signiert ist. Dann auf *Weitere Informationen → Trotzdem ausführen* klicken.
Startest du das Setup erneut, kannst du **aktualisieren** oder **deinstallieren**.

## Manuelle Installation
- **Mit ModLoader:** Den Ordner `TM Vibrant Shaders` aus dem ZIP nach `%LOCALAPPDATA%\TMLoader\database\TmForever\products\` kopieren und die Mod im ModLoader anhaken.
- **Ohne ModLoader:** `TM Vibrant Shaders\<Version>\TMVibrantShaders.dll` in den TrackMania-Ordner kopieren (neben `TmForever.exe`) und in **`d3d9.dll`** umbenennen.

## Im Spiel
| Taste | Funktion |
|---|---|
| `F8` | Shader-Menü öffnen oder schließen |
| `F7` | Shader an/aus (Vorher/Nachher-Vergleich) |
| `F12` | Frame-Capture speichern (hilfreich bei Fehlerberichten) |

- **Preset:** Vibrant, Cinematic, Golden Hour, Event Horizon (Black Hole), Aurora und weitere.
- **Preset per map mood:** Das Preset, das du auf einer Tag-, Abend- oder Nacht-Map wählst, wird für diese Stimmung gemerkt und dort automatisch gesetzt.
- **Eigene Presets:** Werte anpassen, einen Namen eingeben und auf *Save as preset* klicken.
- **Alles wird automatisch gespeichert.** Einstellungen, eigene Presets und das Log liegen in `Dokumente\TrackMania\TMVS\`.
- **Effect quality** (Bereich *Image*): Auf *Low* stellen, wenn die FPS zu niedrig sind, oder das Preset *Performance* wählen.

## Probleme?
- Keine Effekte zu sehen: In `Dokumente\TrackMania\TMVS\tmvs.log` steht, ob Tiefenpuffer und Engine-Hooks gefunden wurden.
- Kantenglättung (MSAA) des Spiels wird automatisch deaktiviert, das FXAA der Shader ersetzt sie.
