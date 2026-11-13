TM Vibrant Shaders - real-time lighting, weather and skies for
TrackMania Nations / United Forever & TrackMania 2 (ManiaPlanet)
https://github.com/cheatoskar/TM-Vibrant-Shaders

======================================================================
ENGLISH
======================================================================

INSTALL - the easy way
  1. Unzip this whole zip (right-click it, "Extract All...").
  2. Close the game, then run TM-Vibrant-Shaders-Setup.exe from the
     unzipped folder and choose one of:
       - Install for the TrackMania ModLoader (recommended for TMNF/TMUF:
         https://tomashu.dev/software/tmloader/), then tick
         "TM Vibrant Shaders" in the ModLoader and start the game.
       - Install into TrackMania Nations Forever / United Forever (adds
         d3d9.dll next to TmForever.exe).
       - Install into TrackMania 2 / ManiaPlanet (adds d3d11.dll next to
         ManiaPlanet.exe).
       - "Install into another game folder" lets you pick the game folder
         yourself. Windows asks for admin rights if the game is in
         "Program Files".
       - TrackMania Nations ESWC (experimental): only the unofficial 1.80
         patch (TmNationsESWC.exe 0.1.7.9). Choose "Install into another
         game folder" and pick the folder with TmNationsESWC.exe.
       - TrackMania Sunrise (Windows 10 repack) and TrackMania Original
         (Online Patch v2), experimental: the same way. The game's own
         d3d9.dll (dgVoodoo in Original) is renamed to d3d9.chain.dll and
         stays in use; the uninstall puts it back.

INSTALL - by hand, no .exe
  With the ModLoader:
    Copy the folder "TM Vibrant Shaders" into
      %LOCALAPPDATA%\TMLoader\database\TmForever\products
    (paste that into the Explorer address bar), then tick the mod in the
    ModLoader.
  Without the ModLoader:
    Copy "TM Vibrant Shaders\<version>\TMVibrantShaders.dll" into your
    TrackMania folder (next to TmForever.exe) and rename it to d3d9.dll.
    Use only one of the two ways at a time.
  TrackMania Nations ESWC (unofficial 1.80 patch only):
    Copy the same TMVibrantShaders.dll next to TmNationsESWC.exe and
    rename it to d3d9.dll. Leave other mods' DLLs (dinput8.dll) as they
    are.
  TrackMania Sunrise (Windows 10 repack), TrackMania Original (Online
  Patch v2):
    First rename the game's own d3d9.dll to d3d9.chain.dll, then copy
    TMVibrantShaders.dll next to TmSunrise.exe / TMORIGINAL.EXE as
    d3d9.dll.
  TrackMania 2 / ManiaPlanet:
    Copy d3d11.dll from this zip next to ManiaPlanet.exe and start
    ManiaPlanet 64-bit (the mod is 64-bit only).
  Horizon Client (TM Horizon's in-game menu):
    Use TMVS-<version>-horizon.zip from the same release instead (four
    modules for Horizon's plugins folder, see its README.txt). Not both:
    with the standalone mod and the modules installed, only one runs.

IN GAME
  F8   shader menu (presets, every setting, your own presets)
  F7   shaders on/off (TrackMania 2: F5, F7 is its friends list)
  F6   photo mode (Shift+F6: compare split)
  F12  screenshot and frame capture (for bug reports)
  Everything you change is saved automatically, in
  Documents\TrackMania\TMVS (TrackMania 2:
  Documents\ManiaPlanet\TMVS).

UNINSTALL
  Run TM-Vibrant-Shaders-Setup.exe and choose "Uninstall",
  or delete the "TM Vibrant Shaders" folder from the ModLoader products /
  the d3d9.dll from the game folder / the d3d11.dll from the
  ManiaPlanet folder.

VIRUS WARNING?
  The setup is not code-signed, so Windows SmartScreen may warn about it
  ("More info" -> "Run anyway"). It only copies the files from this zip.
  If you would rather not run it, use the "by hand" way above.
  The release page lists a SHA-256 checksum for every file.

======================================================================
DEUTSCH
======================================================================

INSTALLIEREN - der einfache Weg
  1. Die ganze ZIP entpacken (Rechtsklick, "Alle extrahieren...").
  2. Das Spiel schließen, TM-Vibrant-Shaders-Setup.exe aus dem entpackten
     Ordner starten und auswählen:
       - Für den TrackMania ModLoader installieren (empfohlen für TMNF/TMUF:
         https://tomashu.dev/software/tmloader/), dann im ModLoader
         "TM Vibrant Shaders" anhaken und das Spiel starten.
       - In TrackMania Nations Forever / United Forever installieren (kopiert
         d3d9.dll neben die TmForever.exe).
       - In TrackMania 2 / ManiaPlanet installieren (kopiert d3d11.dll
         neben die ManiaPlanet.exe).
       - Mit "Install into another game folder" wählst du den Ordner mit der
         TmForever.exe oder ManiaPlanet.exe selbst. Liegt das Spiel unter
         "Programme", fragt Windows nach Admin-Rechten.
       - TrackMania Nations ESWC (experimentell): nur der inoffizielle
         Patch 1.80 (TmNationsESWC.exe 0.1.7.9). "Install into another
         game folder" wählen und den Ordner mit der TmNationsESWC.exe.

INSTALLIEREN - von Hand, ohne .exe
  Mit ModLoader:
    Den Ordner "TM Vibrant Shaders" nach
      %LOCALAPPDATA%\TMLoader\database\TmForever\products
    kopieren (in die Adresszeile des Explorers einfügen), dann die Mod im
    ModLoader anhaken.
  Ohne ModLoader:
    "TM Vibrant Shaders\<Version>\TMVibrantShaders.dll" in den
    TrackMania-Ordner kopieren (neben TmForever.exe) und in d3d9.dll
    umbenennen. Bitte nur einen der beiden Wege gleichzeitig nutzen.
  TrackMania Nations ESWC (nur der inoffizielle Patch 1.80):
    Dieselbe TMVibrantShaders.dll neben die TmNationsESWC.exe kopieren
    und in d3d9.dll umbenennen. DLLs anderer Mods (dinput8.dll) bleiben
    unverändert.
  TrackMania 2 / ManiaPlanet:
    d3d11.dll aus dieser ZIP neben die ManiaPlanet.exe kopieren und
    ManiaPlanet 64-bit starten (der Mod gibt es nur in 64-bit).

IM SPIEL
  F8   Shader-Menü (Presets, alle Einstellungen, eigene Presets)
  F7   Shader an/aus (TrackMania 2: F5, F7 ist dort die Freundesliste)
  F6   Fotomodus (Shift+F6: Vergleichsansicht)
  F12  Screenshot und Frame-Capture (für Fehlerberichte)
  Alles wird automatisch gespeichert, in Dokumente\TrackMania\TMVS
  (TrackMania 2: Dokumente\ManiaPlanet\TMVS).

DEINSTALLIEREN
  TM-Vibrant-Shaders-Setup.exe starten und "Uninstall" wählen,
  oder den Ordner "TM Vibrant Shaders" aus den ModLoader-Products bzw. die
  d3d9.dll aus dem Spielordner bzw. die d3d11.dll aus dem
  ManiaPlanet-Ordner löschen.

VIRENWARNUNG?
  Das Setup ist nicht signiert, deshalb kann Windows SmartScreen warnen
  ("Weitere Informationen" -> "Trotzdem ausführen"). Es kopiert nur die
  Dateien aus dieser ZIP. Wer es nicht starten möchte, nimmt den Weg
  "von Hand". Auf der Release-Seite steht für jede Datei eine
  SHA-256-Prüfsumme.
