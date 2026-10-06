# TM Vibrant Shaders – Anleitung (Deutsch)

[Zurück zum README](../README.md) · [English](install.md)

Echtzeit-Licht, Wetter und Himmel für TrackMania Nations Forever und United Forever: Schatten, Ambient Occlusion, Lichtstrahlen, Bounce Light, leuchtendes Neon, Regen mit Sound und Gewitter, nasse Straßen mit Spiegelungen, Wasser, Wolken und neue Himmel. Der Mod zeichnet vor dem HUD, die Oberfläche bleibt also scharf.

## Voraussetzungen

- TrackMania Nations Forever oder United Forever, Windows 10 oder 11
- Eine Grafikkarte mit Shader Model 3.0 (praktisch jede seit 2008)
- Optional: der [TrackMania ModLoader (TMLoader)](https://tomashu.dev/software/tmloader/). Es geht auch ohne.

## Installation mit dem Setup

1. **`TM-Vibrant-Shaders.zip`** von der [Release-Seite](https://github.com/cheatoskar/TM-Vibrant-Shaders/releases/latest) herunterladen und **komplett entpacken**.
2. TrackMania schließen und **`TM-Vibrant-Shaders-Setup.exe`** starten.
3. Auswählen:

   | Button | Wann |
   |---|---|
   | **Install for the TrackMania ModLoader** | Du nutzt den ModLoader. Danach im ModLoader-Profil *TM Vibrant Shaders* anhaken. |
   | **Install into TrackMania Nations Forever** / **United Forever** | Ohne ModLoader. Das Setup zeigt jedes gefundene TrackMania mit eigenem Button und legt eine `d3d9.dll` neben die `TmForever.exe`. |
   | **Install into another game folder** | Dein Spiel steht nicht in der Liste. Den Ordner mit der `TmForever.exe` selbst auswählen. |

4. Spiel starten, **`F8`** öffnet das Menü.

Liegt das Spiel unter *Programme*, fragt Windows nach Admin-Rechten. Warnt Windows SmartScreen, liegt das nur daran, dass das Setup nicht signiert ist: *Weitere Informationen → Trotzdem ausführen*. Das Setup kopiert nur die Dateien aus der ZIP.

## Installation von Hand

- **Mit ModLoader:** Den Ordner `TM Vibrant Shaders` aus der ZIP nach `%LOCALAPPDATA%\TMLoader\database\TmForever\products\` kopieren (in die Adresszeile des Explorers einfügen) und die Mod im ModLoader anhaken.
- **Ohne ModLoader:** `TM Vibrant Shaders\<Version>\TMVibrantShaders.dll` neben die `TmForever.exe` kopieren und in **`d3d9.dll`** umbenennen.

Nur einen der beiden Wege nutzen. Eine andere `d3d9.dll` (zum Beispiel ReShade) kann nicht gleichzeitig im selben Spielordner liegen.

## Aktualisieren und deinstallieren

- **Aktualisieren:** Das Setup der neuen Version starten und denselben Button wie beim ersten Mal wählen. Einstellungen und eigene Presets bleiben.
- **Deinstallieren:** Setup starten und *Uninstall* wählen. Einstellungen liegen weiter in `Dokumente\TrackMania\TMVS\`; den Ordner löschen, um sie auch zu entfernen.

## Im Spiel

| Taste | Funktion |
|---|---|
| `F8` | Menü öffnen oder schließen |
| `F7` | Shader an/aus (Vorher/Nachher) |
| `F12` | Screenshot und Frame-Capture speichern (für Fehlerberichte) |

- **Simple-Ansicht:** Himmel, Look, Wetter und Performance als aufklappbare Bereiche. **Advanced:** jede Einstellung, eigene Presets und was jeder Effekt auf deiner Grafikkarte kostet.
- **Presets:** Vibrant, Realistic, Golden Hour, Dreamy, Neon, Horizon, Aurora, Rainy Day, Storm, Replay Cinema, Competition, Performance.
- **Preset pro Map-Stimmung:** Standardmäßig Vibrant auf Tag-Maps, Golden Hour bei Sonnenuntergang, Horizon nachts. Unter *Which preset for which maps* änderbar.
- **Eigene Presets:** In *Advanced* Werte ändern, Namen eingeben, *Save as preset*. Presets sind Textdateien in `Dokumente\TrackMania\TMVS\presets\` und lassen sich weitergeben.
- **Alles wird automatisch gespeichert**, in `Dokumente\TrackMania\TMVS\`.

## Empfohlene Spieleinstellungen

Im TrackMania-Launcher unter *Erweitert*: **Kantenglättung aus** (der Mod schaltet sie sowieso ab und bringt FXAA und TAA mit), **anisotrope Filterung 16x**, Shader-Qualität PC3 High.

## Probleme?

| Problem | Lösung |
|---|---|
| Kein Menü mit `F8` | Mod nicht geladen: im ModLoader angehakt? Liegt die `d3d9.dll` im Ordner des Spiels, das du startest? |
| Menü da, aber keine Effekte | *Enabled* anhaken (`F7`). Steht im Menü eine rote Meldung, in `Dokumente\TrackMania\TMVS\tmvs.log` nach Zeilen mit `depth:` schauen. |
| Effekte liegen über dem HUD | Unbekannte Spielversion. Unterstützt sind TMNF und TMUF 2.11.26. |
| Kein Regen- oder Donner-Sound | Nur bei Regen oder Blitzen, im Rennen und wenn das Spiel im Vordergrund ist. Lautstärke im Menü (*Rain & thunder sound*, bis 2) und im Windows-Lautstärkemixer (*TmForever*). Die Sound- und Musikregler im Spiel wirken nicht darauf. |
| Zu wenig FPS | *Auto quality* an lassen, Preset *Performance* oder *Effect quality: Low*. In *Advanced → Performance* steht, was am meisten kostet. |
| Kanten zackiger als vorher | Absicht: Die Kantenglättung des Spiels muss für die Tiefeneffekte aus sein. FXAA und TAA ersetzen sie. |

Fehler melden: [GitHub Issues](https://github.com/cheatoskar/TM-Vibrant-Shaders/issues), mit `tmvs.log` und bei Bildfehlern einem `F12`-Capture (`screen_*.bmp` und `capture_*.tmcap`). Mehr in [troubleshooting.md](troubleshooting.md).
