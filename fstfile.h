#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mp {
// Parsed FL Studio preset (.fst). .fst files are an FLhd/FLdt chunk pair
// wrapping FL's tagged event stream: event 201 carries the plug-in name,
// 203 the preset name and 213 the raw plug-in state chunk — the same bytes
// SaveRestoreState produces for Fruity plug-ins and the component state for
// plug-ins FL hosts natively.
struct FstChunk {
    std::wstring pluginName;
    std::wstring presetName;
    std::vector<uint8_t> data;
};
// Returns false when the file is missing, malformed or carries no state chunk.
bool ParseFstFile(const std::wstring& path, FstChunk& out);
} // namespace mp
