# OverlapFix in TrackMania 2: Machbarkeit (nur lesend)

Stand: nicht gebaut. Ohne eine Aufnahme der Block-Draws kann ich nicht belegen, dass ein Draw einem Block
zuordenbar ist, und ohne Spiel kann ich keine machen. Ich baue keinen Umweg auf Verdacht.

## Wie TMNF es macht (was in TM2 fehlt)

TMNF kennt zu jedem Draw den Block: der Draw reicht das CHmsCorpus durch, dessen Item zeigt auf die
CSceneMobil des Blocks, und das Block-Array der Karte (CGameCtnChallenge +0x54) liefert Platzierungsreihenfolge
und Einheiten. Daraus wird ein Rang und ein Tiefen-Bias pro Draw (blockorder.cpp, overlapBiasBegin).
Das hängt an einem Hook in die Engine (Draw-Pfad), den es in TM2 nicht geben darf (Anti-Cheat).

## Was der d3d11-Proxy in TM2 sieht

- Pro Draw: Pixel-Shader-Name, Render-Ziele, Texturen, Zustände (capture trace.txt). Keine Block-ID.
- Konstantenpuffer: der Proxy liest SceneV/SceneP und den CarSkin-ObjectV (Auto). Für Block-Shaders
  (`Tech3/Block_TDSN_DefWrite_p` usw.) ist weder das Layout des Objekt-Puffers noch eine Instanzierung
  bekannt. Die GpuCache-Zip mit dem Vertex-Shader liegt hier nicht vor, und trace.txt hat keine
  Instanzzahl.
- Aus der Aufnahme capture_034 (Oct 17): 377 Draws von `Block_TDSN_DefWrite_p` verteilen sich auf nur 35
  verschiedene Texturgruppen (bis zu 46 Draws je Gruppe). Die Engine sortiert also nach Material, nicht
  nach Block. Ob ein Draw ein Block, mehrere Blöcke (Instancing, zusammengelegte Geometrie) oder nur ein
  Teil eines Blocks ist, geht aus den Daten nicht hervor.

## Was für eine Umsetzung nötig wäre (alles lesend)

1. Pro Draw die Weltposition der Geometrie: der Objekt-Konstantenpuffer (Weltmatrix) der Block-Shader oder
   ein Instanzpuffer. Das müsste der Proxy beim Schreiben des Puffers (Map/UpdateSubresource auf seiner
   Seite des Aufrufs) auf der CPU mitschreiben; ein GPU-Readback je Draw (wie beim Auto, einmal pro Frame)
   würde bei Hunderten Draws die GPU anhalten.
2. Das Block-Array der Karte aus ManiaPlanet.exe lesen: bekannt sind App-Global RVA 0x1C8B410, RootMap
   +0x378, CGameCtnChallenge 0x03043000 (Größe 0x748), aber nicht der Offset des Block-Arrays und der
   Block-Felder (Koordinate, Typ, Reihenfolge). Die Reflexionstabellen liefern ihn (wie bei maplook.cpp), ohne
   Spiel prüfbar nur über den Disassembler.
3. Draw und Block zusammenbringen: Weltposition gegen Rasterzelle (Stadium 32 x 8 x 32 m) plus Block-Größe.
   Bei Instancing oder zusammengelegter Geometrie (mehrere Blöcke in einem Draw) entfällt das, und ein
   Bias gilt immer für einen ganzen Draw: zwei überlappende Blöcke im selben Draw lassen sich nicht
   unterscheiden.
4. Dann erst der eigentliche Teil: Rasterizer-State mit DepthBias für Draws mit Rang > 0 (Reverse-Z mit Float-
   Tiefe: positives Bias holt nach vorn; Zustand des Spiels danach zurücksetzen). Das wäre im Proxy erlaubt.

## Verworfen

Bias nach Draw-Reihenfolge (später gezeichnet = vorn): die Engine sortiert nach Material, die Reihenfolge hat
mit der Platzierung nichts zu tun, und es würde auch Flächen innerhalb eines Blocks verschieben.

## Nächster Schritt, wenn ich es angehen soll

Eine Aufnahme mit ergänzter Spur (nur lesend, im Proxy): je Block-Draw die Instanzzahl (Draw-Argumente),
die Größen der Vertex-Konstantenpuffer und Vertex-Puffer, und bei einem Dutzend Draws die ersten 256 Byte
des Objekt-Puffers. Damit lässt sich Punkt 1 und 3 beantworten. Dazu brauche ich einen Lauf von dir auf einer
Karte mit überlappenden Blöcken (Luftmodus oder Geisterblöcke im Editor). Die Spur wäre eine Änderung in
hooks.cpp und capture.cpp, deshalb habe ich sie nicht angefasst (Merge mit dem Modpack-Zweig).
