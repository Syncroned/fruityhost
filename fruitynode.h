#pragma once
#include "node.h"

namespace mp {

// Loads an Image-Line "Fruity" effect DLL (*_x64.dll exporting
// CreatePlugInstance) and wraps it as a graph node. Experimental: the Fruity
// host protocol is only partially documented — see native/probe/README.md.
std::shared_ptr<Node> CreateFruityNode(int id, const std::wstring& modulePath, HostServices* host, std::wstring& error);
// True once any Fruity DLL has been loaded into this process.
bool AnyFruityModuleLoaded();

} // namespace mp
