#include "fruitynode.h"
#include <windows.h>
#include <commdlg.h>
#include <cstdio>
#include <string>

using namespace mp;

struct TinyHost : HostServices {
    void AudioBarrier() override {}
    void RequestRecompile() override {}
    double SampleRate() const override { return 48000; }
    int MaxBlock() const override { return 512; }
};

static std::shared_ptr<Node> g_node;
static bool g_editorOpened;
static float g_inL[512], g_inR[512], g_outL[512], g_outR[512];

void CALLBACK PumpAudio(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_node) return;
    const float* inCh[2] = {g_inL, g_inR};
    float* outCh[2] = {g_outL, g_outR};
    ProcessContext ctx{};
    ctx.frames = 512;
    ctx.in[0] = inCh; ctx.out[0] = outCh;
    ctx.inChannels[0] = 2; ctx.outChannels[0] = 2;
    g_node->Process(ctx);
    g_node->Idle();
    if (g_editorOpened && !g_node->EditorOpen()) PostQuitMessage(0);
}

std::wstring PickDll() {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Fruity plug-ins (*_x64.dll)\0*_x64.dll\0All files\0*.*\0";
    ofn.lpstrTitle = L"Select a Fruity plug-in";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    WIN32_FIND_DATAW fd{};
    std::wstring initial;
    HANDLE h = FindFirstFileW(L"C:\\Program Files\\Image-Line\\FL Studio*\\", &fd);
    while (h != INVALID_HANDLE_VALUE) {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            initial = L"C:\\Program Files\\Image-Line\\" + std::wstring(fd.cFileName) + L"\\Plugins\\Fruity";
        if (!FindNextFileW(h, &fd)) break;
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    if (!initial.empty()) ofn.lpstrInitialDir = initial.c_str();
    return GetOpenFileNameW(&ofn) ? path : L"";
}

int wmain(int argc, wchar_t** argv) {
    std::wstring path = argc > 1 ? argv[1] : PickDll();
    if (path.empty()) { wprintf(L"no plug-in selected\n"); return 0; }

    TinyHost host;
    std::wstring error;
    g_node = CreateFruityNode(1, path, &host, error);
    if (!g_node) { fwprintf(stderr, L"load failed: %ls\n", error.c_str()); return 1; }
    wprintf(L"loaded %ls\n", path.c_str());
    g_node->Prepare(48000, 512);

    std::wstring title = path.substr(path.find_last_of(L"\\/") + 1) + L" - fruityhost";
    g_editorOpened = g_node->OpenEditor(nullptr, title.c_str());
    if (!g_editorOpened) {
        fwprintf(stderr, L"editor failed: %ls\n", g_node->lastError.c_str());
        wprintf(L"(continuing without editor - press Ctrl+C to quit)\n");
    }

    HWND dummy = CreateWindowExW(0, L"Static", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    SetTimer(dummy, 1, 10, PumpAudio);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g_node.reset();
    return 0;
}
