TM Vibrant Shaders as Horizon modules
=====================================

Four plugins for Horizon Client's in-game menu (Horizon Host 0.3.1, plugin ABI 1.4;
they also load in 0.2.0 and newer):

  tmvs             TM Vibrant Shaders: the shaders, presets, colour looks, the photo look
  tmvs-capture     Photos, clips (also of each checkpoint's sector), the video export
  tmvs-upscaling   Render scale with AMD FSR 1, also without the shaders
  tmvs-packs       Resource packs on every map (Nations/United Forever only)

Each one has its own page in the Horizon menu (its tabs), its own switch and its keys in Horizon's
Hotkeys list. The tmvs folder also holds tmvs-core.dll, which all four use: keep it, even if you
switch the TMVS module itself off.

Install
-------
Copy the four folders into Horizon's plugins folder:

  TrackMania Nations/United Forever (TMLoader):
    %LOCALAPPDATA%\TMLoader\database\TmForever\products\Horizon Host\<version>\plugins
  TrackMania Nations ESWC, Sunrise (Windows 10 repack), Original (Online
  Patch v2) (Horizon's own loader):
    <game folder>\horizon\plugins
  TrackMania 2 has no Horizon: use the standalone TM-Vibrant-Shaders.zip there.

Start the game. The modules are in the Horizon menu (Insert).

Not together with the standalone TM Vibrant Shaders
---------------------------------------------------
The standalone mod (the "TM Vibrant Shaders" ModLoader product, or d3d9.dll in the game folder) and
these modules are the same mod (modules 2.0.0 = the standalone 1.6.0 core). If both are installed, only one runs: the one the game loads first,
usually the standalone. The other stays off and says so (tmvs.log, Horizon's log, the module's page).
Remove one of them.

Both use the same settings and presets (Documents\TrackMania\TMVS, clips in Documents\TrackMania\TMClips):
switching from one to the other keeps your look, your keys and your clips settings.

Logs: Documents\TrackMania\TMVS\tmvs.log (ESWC, Sunrise, Original: tmvs-eswc.log,
tmvs-sunrise.log, tmvs-original.log), and Horizon's log (Horizon menu, Log).
