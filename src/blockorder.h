#pragma once
#include <cstdint>

// BlockOrder: which block the engine is drawing, and where it was placed among the blocks
// that overlap it. Blocks that share a grid unit (TMUnlimiter maps stack blocks in one cell,
// a turbo road on a platform) have coplanar surfaces and z-fight; the render side pushes the
// block placed last towards the camera by a small depth bias scaled with this rank. Action blocks
// rank above normal blocks whatever the placement order, in the order engine off, boosters, then
// start / finish / checkpoint / multilap; of two blocks of the same kind the later placed wins.
//
// The engine side (Ghidra, TMNF names): CGameCtnChallenge +0x54 is a CFastBuffer<CGameCtnBlock*>
// (count, data), CreateBlock appends, RemoveBlock shifts the rest down, so the array is the
// placement order (for a loaded map: the file's order). A block holds its units at +0x28
// (CFastArray of CGameCtnBlockUnit, offsets +0x28/+0x2c/+0x30 added to the block's coordinate
// +0x48/+0x4c/+0x50) and its CSceneMobil at +0x30. The mobil's CHmsItem is at +0x28 and the item
// points back at the mobil (+0x40). A draw call (CDx9ShaderKeeper::UpdateAndRender) gets the
// CHmsCorpus, whose item is at +0x48. So the draw maps to its block through the item pointer.
namespace blockorder {

// The depth bias rank of the block the engine is drawing right now (called from the D3D9 device
// wrapper before each draw of the game's own scene): 0 = not a block, or a block that overlaps no
// other block (no bias); 1..7 = its place among the blocks that overlap at its spot, later placed
// = higher (action blocks above normal ones). Cheap: called for every draw call.
int currentDrawRank();

// Map loaded / block removed / editor changes: rebuild the overlap groups (call sites in engine.cpp).
void invalidate();

// For engine.cpp: CMwId::GetString (block names, for the action blocks below), once at startup.
void setIdNameFunction(uintptr_t address);

// For engine.cpp. The current map (null: none); a new map rebuilds the groups on its next draw.
void setChallenge(const void* challenge);

// The engine's draw hooks: beginDraw(corpus) before the engine draws (a CHmsCorpus, null when the
// call has none) sets what currentDrawRank() answers and returns the previous rank; endDraw(previous)
// after the draw puts it back, so nothing else inherits it.
int beginDraw(const void* corpus, bool delayedPass);
void endDraw(int previous);

} // namespace blockorder
