# TM Vibrant Shaders – Anleitung (Deutsch)

[Zurück zum README](../README.md) · [English](install.md)

Echtzeit-Licht, Wetter und Himmel für TrackMania Nations Forever, United Forever und TrackMania 2 (ManiaPlanet): Schatten, Ambient Occlusion, Lichtstrahlen, Bounce Light, leuchtendes Neon, Regen mit Sound und Gewitter, nasse Straßen mit Spiegelungen, Wasser, Wolken und neue Himmel. Der Mod zeichnet vor dem HUD, die Oberfläche bleibt also scharf.

## Voraussetzungen

- TrackMania Nations Forever oder United Forever, oder TrackMania 2 (ManiaPlanet 4, 64-bit, siehe [TrackMania 2](#trackmania-2-maniaplanet-4)), Windows 10 oder 11
- Eine Grafikkarte mit Shader Model 3.0 (praktisch jede seit 2008)
- Optional: der [TrackMania ModLoader (TMLoader)](https://tomashu.dev/software/tmloader/). Es geht auch ohne.

## Mit dem ModLoader: installiert und aktualisiert sich selbst

Wer TrackMania Nations/United Forever über den ModLoader startet, lässt ihn den Mod und jedes Update holen:

1. ModLoader schließen.
2. `%LOCALAPPDATA%\TMLoader\config.yaml` mit dem Editor öffnen (Pfad in die Adresszeile des Explorers einfügen).
3. Unter `servers:` diese Zeile ergänzen (eingerückt wie die anderen) und speichern:

   ```yaml
     - 'https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/'
   ```

4. ModLoader starten, *TM Vibrant Shaders* im Profil anhaken, Spiel starten.

Neue Versionen kommen dann über das Auto-Update des ModLoaders, ohne Download. Vorabversionen: stattdessen `https://cheatoskar.github.io/TM-Vibrant-Shaders/modloader/beta/` eintragen (nur eine der beiden Adressen). Für TrackMania 2 gibt es keinen ModLoader: dort das Setup nehmen.

## Installation mit dem Setup

1. **`TM-Vibrant-Shaders.zip`** von der [Release-Seite](https://github.com/cheatoskar/TM-Vibrant-Shaders/releases/latest) herunterladen und **komplett entpacken**.
2. TrackMania schließen und **`TM-Vibrant-Shaders-Setup.exe`** starten.
3. Auswählen:

   | Button | Wann |
   |---|---|
   | **Install for the TrackMania ModLoader** | Du nutzt den ModLoader. Danach im ModLoader-Profil *TM Vibrant Shaders* anhaken. |
   | **Install into TrackMania Nations Forever** / **United Forever** | Ohne ModLoader. Das Setup zeigt jedes gefundene TrackMania mit eigenem Button und legt eine `d3d9.dll` neben die `TmForever.exe`. |
   | **Install into TrackMania 2 (ManiaPlanet)** | Das Setup legt eine 64-bit-`d3d11.dll` neben die `ManiaPlanet.exe`. Siehe [TrackMania 2](#trackmania-2-maniaplanet-4). |
   | **Install into another game folder** | Dein Spiel steht nicht in der Liste. Den Ordner mit der `TmForever.exe` oder `ManiaPlanet.exe` selbst auswählen. |

4. Spiel starten, **`F8`** öffnet das Menü.

Liegt das Spiel unter *Programme*, fragt Windows nach Admin-Rechten. Warnt Windows SmartScreen, liegt das nur daran, dass das Setup nicht signiert ist: *Weitere Informationen → Trotzdem ausführen*. Das Setup kopiert nur die Dateien aus der ZIP.

## Installation von Hand

- **Mit ModLoader:** Den Ordner `TM Vibrant Shaders` aus der ZIP nach `%LOCALAPPDATA%\TMLoader\database\TmForever\products\` kopieren (in die Adresszeile des Explorers einfügen) und die Mod im ModLoader anhaken.
- **Ohne ModLoader:** `TM Vibrant Shaders\<Version>\TMVibrantShaders.dll` neben die `TmForever.exe` kopieren und in **`d3d9.dll`** umbenennen.

Nur einen der beiden Wege nutzen. Eine andere `d3d9.dll` (zum Beispiel ReShade) kann nicht gleichzeitig im selben Spielordner liegen. Mit anderen ModLoader-Mods läuft der Mod zusammen, auch mit dem Tacho-Mod CenterSpeed: Dann öffnet `F5` das Menü, weil CenterSpeed `F8` belegt.

## TrackMania 2 (ManiaPlanet 4)

Dieselbe ZIP enthält eine `d3d11.dll` für ManiaPlanets Direct3D-11-Renderer (nur 64-bit). Der ModLoader wird nicht gebraucht.

- **Installieren:** ManiaPlanet schließen, das Setup starten, Button **Install into TrackMania 2 (ManiaPlanet)**. Oder von Hand: `d3d11.dll` aus der ZIP neben die `ManiaPlanet.exe` kopieren. ManiaPlanet **64-bit** starten; oben im Bild erscheint eine Meldung, dass der Mod läuft. Startet der Launcher `ManiaPlanet32.exe`, im Launcher auf 64-bit umstellen. Eine vorhandene `d3d11.dll` (ReShade, anderer Mod) sichert das Setup als `d3d11.dll.tmvs-backup` und stellt sie beim Deinstallieren wieder her.
- **Tasten:** `F8` Menü, `F6` Fotomodus, `F12` Capture, aber **`F5`** schaltet die Effekte an/aus: `F7` ist in ManiaPlanet die Freundesliste. Die Tasten lassen sich im Menü ändern (*Settings*, *Hotkeys*).
- **Deinstallieren:** Setup erneut starten und *Uninstall* wählen, oder die `d3d11.dll` aus dem Spielordner löschen. Einstellungen liegen in `Dokumente\ManiaPlanet\TMVS\`.
- **Online:** Der Mod ändert nur das Bild. Er liest Kamera, Sonne, Position des Autos und die Kommentare der Map und schreibt nichts in den Speicher des Spiels, außer im Map-Editor die Map-Kommentare, wenn du *Set as Author's Shader* drückst. Ob Server oder Wettbewerbe eine `d3d11.dll`-Mod erlauben, ist nicht bestätigt: Regeln dort prüfen. Mehr (englisch): [maniaplanet/README.md](../maniaplanet/README.md#online-openplanet).

## Aktualisieren und deinstallieren

- **Aktualisieren:** Mit dem Update-Server im ModLoader nichts zu tun. Sonst das Setup der neuen Version starten und denselben Button wie beim ersten Mal wählen. Einstellungen und eigene Presets bleiben.
- **Deinstallieren:** Setup starten und *Uninstall* wählen. Einstellungen liegen weiter in `Dokumente\TrackMania\TMVS\`; den Ordner löschen, um sie auch zu entfernen.

## Im Spiel

| Taste | Funktion |
|---|---|
| `F8` | Menü öffnen oder schließen (`F5`, wenn CenterSpeed geladen ist) |
| `F7` | Shader an/aus (Vorher/Nachher); in TrackMania 2 `F5`, dort ist `F7` die Freundesliste |
| `F12` | Screenshot und Frame-Capture speichern (für Fehlerberichte) |
| `F6` | Fotomodus an/aus (geht mit der freien Kamera, Cam 7, von selbst an): kein HUD, volle Qualität. Darin speichert `F12` ein Foto ohne HUD (`photo_NNN.png`), `Shift+F12` ein größeres aus Kacheln (Replay vorher pausieren), Mittelklick setzt den Fokus, `Strg`+Mausrad die Unschärfe, `Shift`+Mausrad zoomt (bis 8x), `Shift+F6` teilt das Bild zum Vergleich: Spiel ohne und mit Mod (Linie mit `Strg`+`Shift`+Mausrad verschieben) |

- **Shaders:** Alle Presets als Karten mit Bild und GPU-Last. Mit der Maus kurz auf einer Karte bleiben zeigt den Look direkt im Spiel, Klick übernimmt ihn. **Customize:** Himmel, Look und Wetter, dazu Farblooks aus `.cube`-LUT-Dateien (Ordner `Dokumente\TrackMania\TMVS\luts`). **Studio:** jede Einstellung. **Maps:** Preset pro Map-Stimmung, Author's Shader. **Performance:** Auto Quality und was jeder Effekt kostet.
- **Presets:** Vibrant, Realistic, Golden Hour, Dreamy, Neon, Horizon, Aurora, Rainy, Storm, Snowstorm, Horror, Replay Cinema, Competition, Performance.
- **Preset pro Map-Stimmung:** Standardmäßig Vibrant auf Tag-Maps, Golden Hour bei Sonnenuntergang, Horizon nachts. Auf der Seite *Maps* änderbar, oder per Rechtsklick auf ein Preset.
- **Eigene Presets:** Werte ändern, dann die *+*-Karte unter *Your presets*: Name und, wenn du willst, eine Zeile Beschreibung eingeben. Das Spiel hinter dem Menü wird das Vorschaubild. Rechtsklick auf die Karte: Beschreibung ändern, neues Bild, löschen. Ein Preset sind zwei Dateien in `Dokumente\TrackMania\TMVS\presets\`: `<Name>.ini` und `<Name>.jpg`. Zum Weitergeben beide Dateien schicken; *Open the presets folder* öffnet den Ordner. Neue Dateien dort erscheinen ohne Neustart. Einen Look kannst du auch als eine Zeile Text teilen: *Copy share code* auf der Seite *Shaders*, beim anderen *Paste share code*.
- **Alles wird automatisch gespeichert**, in `Dokumente\TrackMania\TMVS\`.

## Empfohlene Spieleinstellungen

Im TrackMania-Launcher unter *Erweitert*: **Kantenglättung aus** (der Mod schaltet sie sowieso ab und bringt SMAA (oder FXAA) und TAA mit), **anisotrope Filterung 16x**, Shader-Qualität PC3 High. Die **FX-Nachbearbeitung** (Post-Processing) des Spiels schaltet der Mod selbst ab, solange er läuft (Bloom und Farbfilter, ab der nächsten Map; *Turn off game FX* auf der Einstellungsseite). Die Einstellung im Launcher bleibt, wie sie ist.

## Probleme?

| Problem | Lösung |
|---|---|
| Kein Menü mit `F8` | Mit CenterSpeed öffnet `F5` das Menü. Sonst ist der Mod nicht geladen: im ModLoader angehakt? Liegt die `d3d9.dll` im Ordner des Spiels, das du startest? |
| Menü da, aber keine Effekte | *Enabled* anhaken (`F7`). Steht im Menü eine rote Meldung, in `Dokumente\TrackMania\TMVS\tmvs.log` nach Zeilen mit `depth:` schauen. |
| Effekte liegen über dem HUD | Unbekannte Spielversion. Unterstützt sind TMNF und TMUF 2.11.26. |
| Kein Regen- oder Donner-Sound | Nur bei Regen oder Blitzen, im Rennen und wenn das Spiel im Vordergrund ist. Lautstärke im Menü (*Rain & thunder sound*, bis 2) und im Windows-Lautstärkemixer (*TmForever*). Die Sound- und Musikregler im Spiel wirken nicht darauf. |
| Zu wenig FPS | *Auto quality* an lassen, Preset *Performance* oder *Effect quality: Low*. Auf der Seite *Performance* steht, was am meisten kostet. |
| Straßen und Stadion weiß ohne Texturen, Flaggen normal | Das Spiel konnte seine Texturen nicht laden (32-Bit-Spiel, 2 GB Adressraum, geteilt mit Overlays und Recordern wie Medal). Spiel neu starten, unnötige Overlays schließen oder die Texturqualität im Launcher senken. Mit `tmvs.log` melden: Zeilen mit `memory:` zeigen, was fehlschlug. |
| Kanten zackiger als vorher | Absicht: Die Kantenglättung des Spiels muss für die Tiefeneffekte aus sein. SMAA (oder FXAA) und TAA ersetzen sie. |

Fehler melden: [GitHub Issues](https://github.com/cheatoskar/TM-Vibrant-Shaders/issues), mit `tmvs.log` und bei Bildfehlern einem `F12`-Capture (`screen_*.bmp` und `capture_*.tmcap`). Mehr in [troubleshooting.md](troubleshooting.md).
