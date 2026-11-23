#include "presetui.h"
#include "node.h"
#include <algorithm>
#include <commdlg.h>
#include <map>

namespace mp {
namespace {
constexpr wchar_t kPropNode[] = L"MP.PresetNode";
constexpr wchar_t kPropOld[] = L"MP.PresetOldProc";
constexpr UINT kIdPresetBase = 40000;
constexpr UINT kIdPresetBrowse = 40999;
constexpr size_t kMaxItems = 150;

// Preset paths listed in the currently open popup, per editor window.
std::map<HWND, std::vector<std::wstring>> g_menuFiles;

bool IsFst(const wchar_t* path) {
    const wchar_t* dot = wcsrchr(path, L'.');
    return dot && !_wcsicmp(dot, L".fst");
}
Node* NodeOf(HWND wnd) { return reinterpret_cast<Node*>(GetPropW(wnd, kPropNode)); }

void CollectFst(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CollectFst(p, out);
        else if (IsFst(fd.cFileName)) out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
void RebuildPopup(HWND wnd, HMENU popup) {
    while (DeleteMenu(popup, 0, MF_BYPOSITION)) {}
    Node* node = NodeOf(wnd);
    auto& files = g_menuFiles[wnd];
    files = node ? node->PresetFiles() : std::vector<std::wstring>{};
    // Labels are paths relative to the common preset directory so Patcher's
    // category folders ("Compression\...", "Filter\...") stay visible.
    std::wstring prefix;
    if (!files.empty()) prefix = files[0].substr(0, files[0].find_last_of(L'\\') + 1);
    for (const auto& f : files)
        while (!prefix.empty() && _wcsnicmp(f.c_str(), prefix.c_str(), prefix.size())) {
            size_t cut = prefix.find_last_of(L'\\', prefix.size() - 2);
            prefix = cut == std::wstring::npos ? L"" : prefix.substr(0, cut + 1);
        }
    if (files.empty())
        AppendMenuW(popup, MF_STRING | MF_GRAYED, 0, L"No presets found");
    for (size_t i = 0; i < files.size() && i < kMaxItems; ++i) {
        std::wstring label = files[i].substr(prefix.size());
        if (label.size() > 4 && !_wcsicmp(label.c_str() + label.size() - 4, L".fst"))
            label.resize(label.size() - 4);
        AppendMenuW(popup, MF_STRING, kIdPresetBase + (UINT)i, label.c_str());
    }
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, kIdPresetBrowse, L"Load .fst file...");
}
LRESULT CALLBACK PresetDropProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC old = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kPropOld));
    if (msg == WM_DROPFILES) {
        // .fst drops belong to the host; anything else stays with the plug-in.
        HDROP d = reinterpret_cast<HDROP>(wp);
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        wchar_t path[MAX_PATH] = {};
        bool fst = false;
        for (UINT i = 0; i < n && !fst; ++i) {
            DragQueryFileW(d, i, path, MAX_PATH);
            fst = IsFst(path);
        }
        if (fst) {
            LRESULT r = 0;
            PresetUiHandle(hwnd, msg, wp, lp, r);
            return r;
        }
        // Fall through without DragFinish: the child proc still owns this drop.
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, kPropNode);
        RemovePropW(hwnd, kPropOld);
    }
    return old ? CallWindowProcW(old, hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}
BOOL CALLBACK SubclassChild(HWND child, LPARAM) {
    if (GetPropW(child, kPropOld)) return TRUE;              // already done
    HWND top = GetAncestor(child, GA_ROOT);
    Node* node = NodeOf(top);
    if (!node) return TRUE;
    SetPropW(child, kPropNode, node);
    SetPropW(child, kPropOld, reinterpret_cast<WNDPROC>(GetWindowLongPtrW(child, GWLP_WNDPROC)));
    SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PresetDropProc));
    DragAcceptFiles(child, TRUE);
    return TRUE;
}
} // namespace

std::vector<std::wstring> FindFstPresets(const std::wstring& pluginName,
                                         const std::vector<std::wstring>& presetRoots) {
    std::vector<std::wstring> out;
    if (pluginName.empty()) return out;
    for (const auto& root : presetRoots) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            // "<root>\<category>\<plug-in>" — e.g. Effects\Patcher\Compression.
            std::wstring dir = root + L"\\" + fd.cFileName + L"\\" + pluginName;
            if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) CollectFst(dir, out);
            // Some installs keep presets directly at "<root>\<plug-in>".
            if (!_wcsicmp(fd.cFileName, pluginName.c_str())) CollectFst(dir, out);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    return out;
}

std::vector<std::wstring> FlPresetRoots(const std::wstring& flRoot) {
    std::vector<std::wstring> roots;
    if (!flRoot.empty()) roots.push_back(flRoot + L"\\Data\\Patches\\Plugin presets");
    wchar_t up[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH))
        roots.push_back(std::wstring(up) + L"\\Documents\\Image-Line\\FL Studio\\Presets\\Plugin presets");
    // Preset dirs of other FL installs (or all installs for non-Fruity nodes).
    if (flRoot.empty()) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(L"C:\\Program Files\\Image-Line\\*", &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                std::wstring p = std::wstring(L"C:\\Program Files\\Image-Line\\") + fd.cFileName + L"\\Data\\Patches\\Plugin presets";
                if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) roots.push_back(p);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return roots;
}

void PresetUiAttach(HWND wnd, Node* node) {
    SetPropW(wnd, kPropNode, node);
    HMENU bar = CreateMenu();
    HMENU popup = CreatePopupMenu();
    AppendMenuW(popup, MF_STRING, kIdPresetBrowse, L"Load .fst file...");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), L"Presets");
    SetMenu(wnd, bar);
    DragAcceptFiles(wnd, TRUE);
}

void PresetUiDeepDrop(HWND wnd) {
    EnumChildWindows(wnd, SubclassChild, 0);
}

bool PresetUiHandle(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    switch (msg) {
    case WM_INITMENUPOPUP: {
        HMENU bar = GetMenu(wnd);
        if (bar && reinterpret_cast<HMENU>(wp) == GetSubMenu(bar, 0)) {
            RebuildPopup(wnd, reinterpret_cast<HMENU>(wp));
            result = 0;
            return true;
        }
        return false;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        Node* node = NodeOf(wnd);
        if (!node) return false;
        auto it = g_menuFiles.find(wnd);
        if (id >= kIdPresetBase && it != g_menuFiles.end() && id - kIdPresetBase < it->second.size()) {
            node->LoadPresetFile(it->second[id - kIdPresetBase]);
            result = 0;
            return true;
        }
        if (id == kIdPresetBrowse) {
            wchar_t path[MAX_PATH] = {};
            OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner = wnd;
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"FL Studio presets (*.fst)\0*.fst\0All files\0*.*\0";
            ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) node->LoadPresetFile(path);
            result = 0;
            return true;
        }
        return false;
    }
    case WM_DROPFILES: {
        HDROP d = reinterpret_cast<HDROP>(wp);
        Node* node = NodeOf(wnd);
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        wchar_t path[MAX_PATH] = {};
        for (UINT i = 0; node && i < n; ++i) {
            DragQueryFileW(d, i, path, MAX_PATH);
            if (IsFst(path)) { node->LoadPresetFile(path); break; }
        }
        DragFinish(d);
        result = 0;
        return true;
    }
    case WM_DESTROY:
        g_menuFiles.erase(wnd);
        return false;
    default:
        return false;
    }
}
} // namespace mp
