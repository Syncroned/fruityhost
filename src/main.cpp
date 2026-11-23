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
static FruityGen* g_gen;
static bool g_editorOpened;
static float g_inL[512], g_inR[512], g_outL[512], g_outR[512];
static float g_peak;
static int g_ticks;
static int g_heldNote = -1;
static intptr_t g_heldVoice;

static const int kScale[] = {60, 62, 64, 67, 69, 72, 74, 76};

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
    for (int i = 0; i < 512; ++i) {
        if (g_outL[i] > g_peak) g_peak = g_outL[i];
        if (-g_outL[i] > g_peak) g_peak = -g_outL[i];
    }
    ++g_ticks;
    if (g_gen && g_gen->IsGenerator() && g_ticks % 40 == 0) {   // ~400ms arpeggio step
        if (g_heldVoice) g_gen->NoteOff(g_heldVoice);
        g_heldNote = kScale[(g_ticks / 40) % 8];
        // feed both paths: voice API (piano-roll) and packed MIDI in
        g_gen->SendMIDI(0x80 | (g_heldNote << 8));                 // note off prev
        g_heldVoice = g_gen->NoteOn(g_heldNote, 0.8f);
        g_gen->SendMIDI(0x90 | (g_heldNote << 8) | (90 << 16));    // note on
    }
    if (g_ticks % 50 == 49) {             // ~500ms level readout
        int bars = (int)(g_peak * 50 + 0.5f);
        printf("\rout peak %5.3f |%-50s| note %d ", g_peak, std::string(bars, '#').c_str(), g_heldNote);
        fflush(stdout);
        g_peak = 0;
    }
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
    g_gen = dynamic_cast<FruityGen*>(g_node.get());
    if (g_gen && g_gen->IsGenerator())
        wprintf(L"generator detected - arpeggio at note 60..76 every 400ms, level meter below\n");

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
