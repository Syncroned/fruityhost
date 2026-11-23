#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace mp {
class Node;

// Every .fst preset visible for a plug-in: scans "<root>\<category>\<plugin>"
// recursively under each supplied "Plugin presets" root (FL install's
// Data\Patches\Plugin presets and the user's Documents preset store).
std::vector<std::wstring> FindFstPresets(const std::wstring& pluginName,
                                         const std::vector<std::wstring>& presetRoots);
// Standard preset roots: the FL installation (when known) plus the user's
// Documents store where FL saves custom presets.
std::vector<std::wstring> FlPresetRoots(const std::wstring& flRoot);

// Attaches a "Presets" menu and .fst drag & drop to a plug-in editor window.
// The popup is rebuilt on open via node->PresetFiles(); choosing an entry or
// dropping a file calls node->LoadPresetFile(). The node pointer travels in a
// window property, so message handling needs no further context.
void PresetUiAttach(HWND wnd, Node* node);
// Call from the editor window's proc before DefWindowProc. Handles the menu,
// preset commands and drops; returns true when the message was consumed.
bool PresetUiHandle(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
// Opts every descendant into drag & drop and forwards .fst drops to the
// editor's preset handling. VCL/VST child windows would otherwise swallow
// them; non-preset files still reach the original child proc.
void PresetUiDeepDrop(HWND wnd);
} // namespace mp
