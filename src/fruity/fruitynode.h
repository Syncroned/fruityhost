#pragma once
#include "node.h"

namespace mp {

// Loads an Image-Line "Fruity" effect DLL (*_x64.dll exporting
// CreatePlugInstance) and wraps it as a graph node. Experimental: the Fruity
// host protocol is only partially documented — see native/probe/README.md.
std::shared_ptr<Node> CreateFruityNode(int id, const std::wstring& modulePath, HostServices* host, std::wstring& error);

// Generator extras: Fruity nodes implement this too — dynamic_cast a node to
// FruityGen to detect instruments and play notes on them.
struct FruityGen {
    virtual ~FruityGen() = default;
    virtual bool IsGenerator() const = 0;
    virtual void SendMIDI(uint32_t msg) = 0;
    // Piano-roll notes (what FL sends to generators): pitch in cents
    // (semitone*100). NoteOn returns a voice handle for NoteOff, 0 on failure.
    virtual intptr_t NoteOn(int semitone, float velocity) = 0;
    virtual void NoteOff(intptr_t voice) = 0;
};
// True once any Fruity DLL has been loaded into this process.
bool AnyFruityModuleLoaded();

} // namespace mp
