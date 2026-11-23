// Image-Line "Fruity" plug-in host node (experimental).
//
// Fruity plug-ins are Delphi DLLs exporting:
//   TFruityPlug* __stdcall CreatePlugInstance(TFruityPlugHost* host, intptr_t tag)
//
// The host/plugin objects are Delphi classes; Delphi x64 stores user virtuals
// at vtable[0..] (TObject's own methods live at negative VMT offsets), so a
// C++ object with matching field/virtual order impersonates them directly.
// Audio is exchanged as interleaved stereo floats (TWAV32FS).
//
// What this implements is the subset verified by native/probe (see its
// README): ~85% of FL Studio 2025 effects construct, render, save state and
// open their VCL editor forms inside a plain Win32 host window.
#include "fruitynode.h"
#include "fstfile.h"
#include "presetui.h"
#include <map>
#include <mutex>
#include <cstring>
#include <cwchar>
#include <windows.h>
#include <winioctl.h>
#include <objbase.h>
#include <shlobj.h>

using IntP = intptr_t;

namespace mp {
namespace {
static void FruityDbg(const char* fmt, ...);
static bool FlRegistered();
// Cached so hot callbacks can cheaply decide whether tracing is on at all.
static const bool gFruityDbgOn = GetEnvironmentVariableW(L"MP_FRUITY_DEBUGLOG", nullptr, 0) != 0;

// SDK translations (see native/probe/fruity_probe.cpp for the instrumented
// version with full protocol notes).
enum { FPN_Param = 0, FPN_ParamValue = 1 };
enum { PI_CantInterpolate = 1, PI_Float = 2, PI_Centered = 4 };
enum {
    FPD_ShowEditor = 0,
    FPD_ProcessMode = 1, FPD_SetBlockSize = 3, FPD_SetSampleRate = 4,
    FPD_SetEnabled = 11, FPD_SetPlaying = 12, FPD_SongPosChanged = 13,
    FPD_SetTimeSig = 14, FPD_SetNumSends = 17, FPD_SetSamplesPerTick = 20,
    FPD_RoutingChanged = 25, FPD_GetParamInfo = 26,
};
enum { FPE_Tempo = 0 };   // PV_ProcessEvent: float BPM bits in Value, samples per tick in Flags
enum { PV_DestroyObject = 0, PV_Dispatcher, PV_Idle, PV_SaveRestore, PV_GetName,
       PV_ProcessEvent, PV_ProcessParam, PV_EffRender, PV_GenRender,
       PV_TriggerVoice, PV_VoiceRelease, PV_VoiceKill, PV_VoiceProcessEvent,
       PV_VoiceRender, PV_NewTick, PV_MIDITick, PV_MIDIIn, PV_MsgIn };
enum { FPF_Generator = 1, FPF_GetNoteInput = 1 << 4 };
enum { REC_UpdateValue = 1, REC_GetValue = 2 };
enum {
    FHD_SetLatency = 30, FHD_GetNumInOut = 50, FHD_GetInstance = 24,
    FHD_GetMainFormHandle = 46, FHD_GetProgPath = 29, FHD_GetProjDataPath = 47,
    FHD_GetMixingTime = 36, FHD_GetPlaybackTime = 37, FHD_GetSelTime = 38,
    FHD_CheckProdCode = 26,
};

#pragma pack(push, 4)
struct FPTime { double t, t2; };
struct TimeSigInfo { int32_t StepsPerBar, StepsPerBeat, PPQ; };
struct IOBuffer { void* Buffer; uint32_t Flags; };
#pragma pack(pop)

// TFruityPlugInfo layout varies per build: Image-Line plug-ins pack fields at
// 4 bytes, SDK-built plug-ins at 8. LongName offset is detected at runtime.
struct FruityInfo {
    const char* longName; const char* shortName;
    int flags, numParams, numOutCtrls;
};
bool ReadInfo(void* raw, FruityInfo& out) {
    if (!raw) return false;
    auto looksString = [](void* p) {
        if ((uintptr_t)p < 0x10000) return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(p, &mbi, sizeof mbi) || !(mbi.State & MEM_COMMIT)) return false;
        auto* s = (const char*)p;
        if (s[0] < 32 || s[0] > 126) return false;
        for (int i = 0; i < 64; ++i) { if (!s[i]) return true; if (s[i] < 9 || s[i] > 126) return false; }
        return false;
    };
    char* base = (char*)raw;
    int pack = looksString(*(void**)(base + 8)) ? 8 : looksString(*(void**)(base + 4)) ? 4 : 0;
    if (!pack) return false;
    // Aligned (8):  LongName@8  ShortName@16 Flags@24 NumParams@28 NumOutCtrls@40
    // Packed  (4):  LongName@4  ShortName@12 Flags@20 NumParams@24 NumOutCtrls@36
    int d = pack == 8 ? 0 : -4;
    out.longName = *(const char**)(base + 8 + d);
    out.shortName = *(const char**)(base + 16 + d);
    out.flags = *(int*)(base + 24 + d);
    out.numParams = *(int*)(base + 28 + d);
    out.numOutCtrls = *(int*)(base + 40 + d);
    return true;
}

// Minimal COM IStream over memory for SaveRestoreState.
class MemStream final : public IStream {
    std::vector<uint8_t> data;
    size_t pos = 0;
    ULONG refs = 1;
public:
    const std::vector<uint8_t>& Bytes() const { return data; }
    void Load(const uint8_t* src, size_t n) { data.assign(src, src + n); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) { *out = this; AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs == 0 ? (delete this, 0) : refs; }
    HRESULT STDMETHODCALLTYPE Read(void* dst, ULONG n, ULONG* got) override {
        ULONG take = (ULONG)std::min<size_t>(n, data.size() - pos);
        memcpy(dst, data.data() + pos, take); pos += take;
        if (got) *got = take;
        return take == n ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* src, ULONG n, ULONG* wrote) override {
        if (pos + n > data.size()) data.resize(pos + n);
        memcpy(data.data() + pos, src, n); pos += n;
        if (wrote) *wrote = n;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER off, DWORD origin, ULARGE_INTEGER* np) override {
        size_t base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? pos : data.size();
        pos = (size_t)std::max<int64_t>(0, (int64_t)base + off.QuadPart);
        if (np) np->QuadPart = pos;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER s) override { data.resize(s.QuadPart); if (pos > data.size()) pos = data.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
};

// TFruityPlugHost impersonation (SDK: fp_plugclass.h — int32 Reserved after
// TempBuffers, nothing else; plug-ins index this struct absolutely, so the
// layout must match exactly). Our private state lives past the end where
// plug-ins never read: Owner/ProgPath/Latency/EditorHwnd/Timing.
struct FruityHost {
    void** vftable;
    int32_t HostVersion;
    int32_t Flags;
    uint64_t AppHandle;
    void* WaveTables[10];
    float* TempBuffers[4];
    int32_t Reserved[30];
    // ---- private tail (not part of TFruityPlugHost) ----
    Node* Owner;
    const char* ProgPath;
    const char* ProjData;
    IntP Latency;
    HWND EditorHwnd;
    struct FruityTiming* Timing;
    bool IsPatcher;
};
struct HostVTable { void* f[96]; };

class FruityNode;
static FruityHost* gCreatingHost;   // trace-only: host being created right now
void BuildHostObject(FruityHost& h, const char* progPath, Node* owner);

// The "service object" returned for host dispatcher messages outside the
// documented set (FHD_71/FHD_86/...): FL 2025 plug-ins request newer services
// during construction but tolerate stubs. All its methods return 1, matching
// the probe configuration that successfully loaded ~85% of effects.
// Delphi interface contract: [0]=QueryInterface (writes self into out, S_OK),
// [1..2]=_AddRef/_Release, [3+]=service methods. Plug-ins call further methods
// on whatever a service method returns, so a scalar answer poisons them —
// Newtone raised EEDFADE on it. Answer with a usable object instead.
static void* serviceVt[96];
static void* serviceObj = serviceVt;
static void* resourceVt[96];
static void* resourceObj = resourceVt;

// ---- FHD_71: plugin-manager/path-manager service ----------------------------
// Patcher is a recursive host: after it LoadLibrarys a sub-plugin module it
// asks this service to instantiate it. Real FL contract (from FLEngine):
//   vt[0](code,flagByte,out0x400)  record query -> -1 found / 0 not
//   vt[2](op,b,c,d)                misc ops     -> 0
//   vt[3](op,...)                  interface query
//   vt[6](0x40F, inModulePtr, mask, d, out0x400) instantiate -> -1
// The out record is the created plugin object's first 0x400 bytes (a Delphi
// object snapshot — its vtable keeps pointing at the plugin's real methods).
// Plug-ins can skip the wrapper's methods entirely and call
// [svc+8]->vt[0](inner, code, flagByte, out0x400) on the inner manager
// object directly — FLEX resolves its packs root this way (code 0x415 ->
// user data path as UTF-16 text in out). Nonzero return = string found.
struct FruityMgrInner { void** vt; FruityHost* host; };
static void* mgrInnerVt[96];
static FruityHost* gAnyHost;
static wchar_t gFlRootW[MAX_PATH];
static wchar_t gFlInstallSharedW[MAX_PATH];
static IntP __stdcall SvcPathQuery(IntP code, IntP flag, IntP out);
IntP __stdcall MgrInnerQuery(FruityMgrInner* self, IntP code, IntP flag, IntP out) {
    return SvcPathQuery(code, flag, out);
}
struct FruityMgrSvc { void** vt; FruityMgrInner* inner; };
static void* mgrSvcVt[96];
// Every service object is {vt, FruityMgrInner*, tag}: plug-ins may skip the
// wrapper and call inner->vt[0] directly (FLEX's pack-dir query), so the
// inner always carries the path-query contract. tag is the FHD id / op code
// that produced the object, for log attribution.
static void** NewSvcObj(void** vt, IntP tag) {
    return new void* [3] { vt, new FruityMgrInner { mgrInnerVt, gAnyHost }, (void*)tag };
}
IntP __stdcall MgrQueryRecord(void*, IntP a, IntP b, IntP out) {
    FruityDbg("  svc71: m0 record code=%llx flag=%llx out=%p\n", (long long)a, (long long)b, (void*)out);
    return 0;
}
IntP __stdcall MgrOp(void*, IntP a, IntP b, IntP c, IntP d, IntP e) {
    FruityDbg("  svc71: m2 op=%llx b=%llx c=%llx d=%llx e=%llx\n", (long long)a, (long long)b, (long long)c, (long long)d, (long long)e);
    // Unknown manager ops get a generic object like other service calls —
    // callers Supports()/method-call into results, a bare 0 poisons them.
    return (IntP)NewSvcObj(serviceVt, a);
}
IntP __stdcall MgrIfQuery(void* self, IntP a, IntP b, IntP c, IntP d, IntP e) {
    FruityDbg("  svc71: m3 ifq op=%llx b=%llx c=%llx d=%llx\n", (long long)a, (long long)b, (long long)c, (long long)d);
    // Interface query by op-code (0x3F4/0x3FE ...): hand back a generic
    // service object like unknown services get.
    return (IntP)NewSvcObj(serviceVt, a);
}
IntP __stdcall MgrCreate(FruityMgrSvc* self, IntP code, IntP inModulePtr,
                         IntP /*mask*/, IntP /*aux*/, IntP outRec) {
    if (code != 0x40F || !inModulePtr || !outRec || !self->inner || !self->inner->host ||
        !self->inner->host->IsPatcher)
        return (IntP)NewSvcObj(serviceVt, code);
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((void*)inModulePtr, &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE)
        return (IntP)NewSvcObj(serviceVt, code);
    auto create = (void*(__stdcall*)(void*, IntP))GetProcAddress((HMODULE)mbi.AllocationBase, "CreatePlugInstance");
    if (!create) return (IntP)NewSvcObj(serviceVt, code);
    auto* host2 = new FruityHost;
    BuildHostObject(*host2, self->inner->host->ProgPath, self->inner->host->Owner);
    host2->EditorHwnd = self->inner->host->EditorHwnd;
    void* plug = nullptr;
    __try { plug = create(host2, 0x7ea1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!plug) return (IntP)NewSvcObj(serviceVt, code);
    FruityDbg("  svc71: create module=%p plug=%p\n", mbi.AllocationBase, plug);
    __try { memcpy((void*)outRec, plug, 0x400); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return -1;
}

// Service methods hand back a fresh probed object per call (mirrors the
// probe): plug-ins store the result and Supports()/method-call into it, so a
// shared or scalar answer breaks Delphi interface checks — Newtone raised on
// exactly that. obj[1] tags the FHD id that produced the object so chained
// service calls can be attributed.
static IntP __stdcall ServiceStubImpl(int slot, void* self, IntP a, IntP b, IntP c) {
    IntP tag = 0;
    if (gFruityDbgOn) __try { tag = ((IntP*)self)[2]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    FruityDbg("  svc[fhd=%lld] m%d a=%p b=%p c=%p -> obj\n", (long long)tag, slot, (void*)a, (void*)b, (void*)c);
    return (IntP)NewSvcObj(serviceVt, tag);
}
template<int N> static IntP __stdcall ServiceStubN(void* self, IntP a, IntP b, IntP c) {
    return ServiceStubImpl(N, self, a, b, c);
}
IntP __stdcall ServiceStub(void*, IntP a, IntP, IntP) {
    FruityDbg("  svc: method a=%p -> obj\n", (void*)a);
    return (IntP)NewSvcObj(serviceVt, 0);
}
IntP __stdcall ServiceRef(void*, IntP, IntP, IntP) { return 1; }
// vtable[0] of a service object is its query call (req -> out). Mirrors probe
// mode 8, the configuration that rendered menus with full captions:
//  * generic services answer 0 and hand themselves back in out[0] — returning 1
//    with an untouched record left menu items blank;
//  * fhd=86 (SynthResModule resource registry: '.CAPTION', 'Menu', ...) answers
//    plain "not found" without writing — out[0] is read as the caption string,
//    so writing our pointer there rendered its bytes as text ("0C??").
// Small-integer req means the caller is doing the code/flag/out record query,
// not passing a name pointer: FLEX resolves its packs root by calling the
// service's vt[0] as vt[0](svc, code, flagByte, out0x400) — code 0x415 is the
// user data root, answered as UTF-16 text. (FruityMgrInner.vt[0] same shape.)
// flag selects the content-search root: 0 is the user data dir
// (Documents\Image-Line), 1 is the factory content dir (<FL>\Data\Patches\
// Packs — FLEX's *.flexpack files live there), 2 the wider <FL>\Data.
static IntP __stdcall SvcPathQuery(IntP code, IntP flag, IntP out) {
    FruityDbg("  svc: path query code=%llx flag=%llx out=%p\n", (long long)code,
              (long long)flag, (void*)out);
    if (!gAnyHost || !out) return 0;
    char path[MAX_PATH] = {};
    if (flag == 1 || flag == 2) {
        const wchar_t* sub = flag == 1 ? L"\\Data\\Patches\\Packs\\" : L"\\Data\\";
        wchar_t w[MAX_PATH];
        swprintf_s(w, L"%s%s", gFlRootW[0] ? gFlRootW : gFlInstallSharedW, sub);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, path, MAX_PATH, nullptr, nullptr);
    } else {
        const char* p = gAnyHost->ProjData ? gAnyHost->ProjData : gAnyHost->ProgPath;
        strcpy_s(path, p);
    }
    // out is a UTF-8/ANSI buffer, not UTF-16: FLEX concatenates its bytes
    // directly and a wide write leaves it reading only the first char.
    __try { strcpy_s((char*)out, 0x400, path); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
IntP __stdcall ServiceLookup(void* self, IntP req, IntP out, IntP out2) {
    if (req > 0 && req < 0x10000) return SvcPathQuery(req, out, out2);
    __try {
        char dump[33] = {};
        if (req) for (int j = 0; j < 32; ++j) { char c = ((const char*)req)[j]; dump[j] = (c >= 32 && c < 127) ? c : '.'; }
        FruityDbg("  svc: query req=%p '%s' out=%p -> self\n", (void*)req, dump, (void*)out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (out) __try { *(void**)out = self; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
IntP __stdcall ResourceLookup(void* /*self*/, IntP req, IntP out, IntP size) {
    if (req > 0 && req < 0x10000) return SvcPathQuery(req, out, size);
    // Resource registration records the answer text into a caller record and
    // caches it for later rendering (menus, dropdowns) — writing an object
    // pointer into out[0] here leaves mojibake in the editor. Echo the
    // untranslated caption: no localization, so the original text is right.
    if (!req || !out || size <= 0) return 0;
    __try {
        auto* s = (const char*)req;
        if (gCreatingHost) {
            char dump[65] = {};
            for (int j = 0; j < 64; ++j) { char c = s[j]; dump[j] = (c >= 32 && c < 127) ? c : '.'; }
            FruityDbg("  res: req=%p '%s' -> echo\n", (void*)req, dump);
        }
        auto* d = (char*)out;
        IntP i = 0;
        for (; i < size - 1 && s[i]; ++i) d[i] = s[i];
        d[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
// Song position/tempo shared between the audio thread (writer, once per block)
// and host callbacks (any thread). fhost.Timing points at the node's instance.
constexpr int kFruityPPQ = 96;
struct FruityTiming { std::atomic<double> ticks{0}, tempo{120}; };
static double SongTicks(FruityHost* self) {
    auto* t = self->Timing;
    return t ? t->ticks.load(std::memory_order_relaxed) : 0.0;
}
// FHD_GetMixingTime/GetPlaybackTime: Index = GT_* time format.
static void FillSongTime(FruityHost* self, IntP format, FPTime* out) {
    auto* t = self->Timing;
    const double ticks = SongTicks(self), tempo = t ? t->tempo.load(std::memory_order_relaxed) : 120.0;
    const double beats = ticks / kFruityPPQ, ms = beats * 60000.0 / tempo;
    switch (format & 0xFF) {
    case 0:  out->t = beats; break;                  // GT_Beats
    case 4:  out->t = ticks; break;                  // GT_Ticks
    default: out->t = ms; break;                     // GT_AbsoluteMS / RunningMS / MSSinceStart
    }
    out->t2 = 0;
}
int __stdcall HostSongMixingTime(FruityHost* self) { return (int)SongTicks(self); }
double __stdcall HostSongTimeExact(FruityHost* self) { return SongTicks(self); }

// Every plug-in call into the DLL is guarded: a fault marks the node failed
// instead of taking down the engine. No destructors may live in these frames.
IntP __stdcall HostDispatcher(FruityHost* self, IntP /*sender*/, IntP id, IntP index, IntP value) {
    if (gFruityDbgOn) FruityDbg("  host: dispatcher id=%d idx=%lld val=%lld\n", (int)id, (long long)index, (long long)value);
    switch (id) {
    case FHD_GetNumInOut: return 1;                                    // one stereo bus
    case FHD_GetMainFormHandle: if (value) *(HWND*)value = self->EditorHwnd; return 1;
    case FHD_GetInstance: return (IntP)GetModuleHandleW(nullptr);
    case FHD_GetMixingTime: case FHD_GetPlaybackTime:
        if (value) FillSongTime(self, index, (FPTime*)value); return 1;
    case FHD_GetSelTime:                                               // no selection
        if (value) { ((FPTime*)value)->t = 0; ((FPTime*)value)->t2 = 0; } return 0;
    // FL 2025 GetPath: Index selects which path (0 = prog root; other GP_*
    // indices are user-data paths — FLEX uses idx 7 for its packs dir).
    case FHD_GetProgPath:
        if (index == 0) return (IntP)self->ProgPath;
        FruityDbg("getpath idx=%lld -> projdata\n", (long long)index);
        return (IntP)(self->ProjData ? self->ProjData : self->ProgPath);
    // FL's project data path is the user's Image-Line folder — FLEX packs,
    // Newtone data, user presets all live under <Documents>\Image-Line.
    case FHD_GetProjDataPath: return (IntP)(self->ProjData ? self->ProjData : self->ProgPath);
    case FHD_CheckProdCode: {
        // The plug-in marks its licence record pending (ORs 0x8000 into flags)
        // and asks the host to attest the product. FL clears the pending bit
        // for licensed products — the return value is ignored. Answer from
        // the machine's real Image-Line registration: a genuine FL install
        // registers under HKCU\Software\Image-Line\Registrations, so licensed
        // machines get the clear and unlicensed ones stay trial.
        if (FlRegistered() && value)
            __try { *(int32_t*)value &= ~0x8000; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 1;
    }
    case FHD_SetLatency: self->Latency = value; return 0;
    case 45: case 19: return 1;                                        // debug log / msg box: swallow
    case 86: return (IntP)NewSvcObj(resourceVt, 86);                                // resource registry
    case 71: {
        auto* inner = new FruityMgrInner { mgrInnerVt, self };
        return (IntP)new FruityMgrSvc { mgrSvcVt, inner };             // plugin manager
    }
    // Documented calls with plain-value answers must not fall through to the
    // service object: e.g. the parameter right-click menu asks for host entries
    // (18) until it gets null — a non-null answer looped forever and froze the UI.
    case 18: return 0;                                                 // GetParamMenuEntry: no host entries
    case 1:  return 0;                                                 // GetParamMenuFlags
    case 0:  return 1;                                                 // ParamMenu (host item chosen)
    case 5: return 0;                                                // WantMIDIInput: accepted
    case 92:
        // Newer GetPath (FL 2025, beyond the SDK's documented ids): Value is
        // an out char buffer the host fills with a path. Index 0 is the user
        // data root — FLEX appends "FLEX\Packs" onto it; a non-string answer
        // leaves it opening relative paths.
        if (value) {
            const char* p = self->ProjData ? self->ProjData : self->ProgPath;
            FruityDbg("getpath92 idx=%lld -> %s\n", (long long)index, p);
            __try { strcpy_s((char*)value, MAX_PATH, p); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return 1;
    case 3: case 9: case 13: case 27: case 33: case 40: return 0;      // names changed, presets, idle, params, thread-safe, captionize
    }
    // Unknown services get a fresh object each (like the probe): plug-ins
    // track them by identity and a shared singleton confused Newtone.
    // obj[1] tags the dispatcher id so later method calls can be attributed.
    return (IntP)NewSvcObj(serviceVt, id);
}
// True when this machine carries a real Image-Line registration
// (HKCU\Software\Image-Line\Registrations\<product>). Result is cached: the
// registry does not change under a running session.
static bool FlRegistered() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Image-Line\\Registrations",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t name[128]; DWORD n = (DWORD)std::size(name);
        if (RegEnumKeyExW(k, 0, name, &n, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
            cached = 1;
        RegCloseKey(k);
    }
    return cached != 0;
}
IntP __stdcall HostStub(void*, IntP, IntP, IntP, IntP) { return 0; }
// Editor edits arrive here (host vtable[1]); flag them so the UI re-reads params.
void __stdcall HostOnParamChanged(FruityHost* self, IntP, IntP, IntP) {
    if (auto* n = self->Owner) n->paramsChanged++;
}
void __stdcall HostGetBuffer(FruityHost* self, IntP, IntP index, IOBuffer* out) {
    if (self == gCreatingHost) FruityDbg("  host: getbuffer idx=%lld out=%p\n", (long long)index, (void*)out);
    if (!out) return;
    out->Buffer = (index >= 0 && index < 4) ? self->TempBuffers[index] : nullptr;
    out->Flags = 0;
}

// Process-wide compatibility fixes for Image-Line's VCL runtime (findings from
// native/probe). The plug-ins cache HWNDs that go stale in our host — their
// helper windows are created/destroyed on paths we don't fully emulate. In FL
// the failing calls below are benign; here each failure makes Delphi's
// Win32Check raise EOSError, which aborts context-menu construction mid-build:
// the popup opens but items never get their captions. Fixes are applied by
// patching the IATs of the plug-in and Image-Line support DLLs only:
//   * DestroyWindow / NtUserDestroyWindow on an already-dead HWND -> success
//     (the requested end state — the window gone — already holds)
//   * PostMessageW to a dead HWND -> success (the target is an ephemeral
//     TPUtilWindow the plug-in recreates on demand)
//   * VirtualFree / NtFreeVirtualMemory refuse to decommit the heap segment
//     holding QuickFontCache's shared anim-timer singleton — Delphi's heap
//     manager otherwise collapses the region while the plug-in still holds a
//     pointer to it (the original crash cascade).
static void* gFruityPinRegion;   // heap address to keep committed
static int gIatHookCount, gDestroyLies, gPostLies, gFreeRefusals;
static void FruityDbg(const char* fmt, ...);

using NtFreeVM_t = LONG(WINAPI*)(HANDLE, PVOID*, ULONG_PTR*, ULONG);
static NtFreeVM_t orig_NtFreeVM;
static LONG WINAPI FruityNtFreeVM(HANDLE proc, PVOID* base, ULONG_PTR* size, ULONG type) {
    if (gFruityPinRegion && base && *(PVOID*)base <= gFruityPinRegion) {
        auto b = (uintptr_t)*base, e = b + (size ? *size : 0);
        if ((uintptr_t)gFruityPinRegion >= b && (uintptr_t)gFruityPinRegion < e) {
            gFreeRefusals++;
            return 0;   // STATUS_SUCCESS — region stays committed
        }
    }
    return orig_NtFreeVM(proc, base, size, type);
}
static BOOL(WINAPI* orig_VirtualFree)(LPVOID, SIZE_T, DWORD);
static BOOL WINAPI FruityVirtualFree(LPVOID addr, SIZE_T sz, DWORD type) {
    if (gFruityPinRegion && addr) {
        auto a = (uintptr_t)addr, w = (uintptr_t)gFruityPinRegion;
        if (w >= a && (sz == 0 || w < a + sz)) { gFreeRefusals++; return TRUE; }   // pretend freed
    }
    return orig_VirtualFree(addr, sz, type);
}
static bool IsDeadHandleError(DWORD e) {
    return e == ERROR_INVALID_WINDOW_HANDLE || e == ERROR_INVALID_MENU_HANDLE || e == ERROR_INVALID_HANDLE;
}
static BOOL(WINAPI* orig_DestroyWindow)(HWND);
static BOOL WINAPI FruityDestroyWindow(HWND h) {
    if (orig_DestroyWindow(h)) return TRUE;
    if (!IsDeadHandleError(GetLastError())) return FALSE;
    gDestroyLies++;
    SetLastError(0);
    return TRUE;
}
static BOOL(WINAPI* orig_NtUserDestroyWindow)(HWND);
static BOOL WINAPI FruityNtUserDestroyWindow(HWND h) {
    if (orig_NtUserDestroyWindow(h)) return TRUE;
    if (IsWindow(h)) return FALSE;
    gDestroyLies++;
    SetLastError(0);
    return TRUE;
}
static BOOL(WINAPI* orig_PostMessageW)(HWND, UINT, WPARAM, LPARAM);
static BOOL WINAPI FruityPostMessageW(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (orig_PostMessageW(h, m, w, l)) return TRUE;
    if (!IsDeadHandleError(GetLastError())) return FALSE;
    gPostLies++;
    SetLastError(0);
    return TRUE;
}
// Delphi's last-resort error path ("Runtime error 216 at ...") goes through
// MessageBox — log the text so a fatal report is not lost headlessly.
// Image-Line's optional shared-library probe ("Could not find oggio
// DLL/dylib at ...") is modal and stalls plug-in loading; the plug-in falls
// back to its built-in code when dismissed. Auto-accept it.
static bool SuppressDllProbe(LPCWSTR text) {
    return text && wcsstr(text, L"DLL/dylib");
}
static bool SuppressDllProbe(LPCSTR text) {
    return text && strstr(text, "DLL/dylib");
}
static int(WINAPI* orig_MessageBoxW)(HWND, LPCWSTR, LPCWSTR, UINT);
static int WINAPI FruityMessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type) {
    FruityDbg("msgbox: [%ls] %ls\n", caption ? caption : L"", text ? text : L"");
    if (SuppressDllProbe(text)) { FruityDbg("msgbox: auto-accepted optional-library probe\n"); return IDOK; }
    return orig_MessageBoxW(h, text, caption, type);
}
static int(WINAPI* orig_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
static int WINAPI FruityMessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type) {
    FruityDbg("msgbox: [%s] %s\n", caption ? caption : "", text ? text : "");
    if (SuppressDllProbe(text)) { FruityDbg("msgbox: auto-accepted optional-library probe\n"); return IDOK; }
    return orig_MessageBoxA(h, text, caption, type);
}
static int(WINAPI* orig_MessageBoxExW)(HWND, LPCWSTR, LPCWSTR, UINT, WORD);
static int WINAPI FruityMessageBoxExW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang) {
    FruityDbg("msgbox: [%ls] %ls\n", caption ? caption : L"", text ? text : L"");
    if (SuppressDllProbe(text)) { FruityDbg("msgbox: auto-accepted optional-library probe\n"); return IDOK; }
    return orig_MessageBoxExW(h, text, caption, type, lang);
}
// File/library access logging: editor-type plug-ins resolve companion
// resources through host-supplied paths; a raise after GetProgPath usually
// means a missing file. Call-through, debug log only.
static HANDLE(WINAPI* orig_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI FruityCreateFileW(LPCWSTR path, DWORD access, DWORD share,
                                       LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tpl) {
    HANDLE r = orig_CreateFileW(path, access, share, sa, disp, flags, tpl);
    FruityDbg("createfile %ls -> %p (err %lu)\n", path ? path : L"<null>", r, GetLastError());
    return r;
}
static HANDLE(WINAPI* orig_FindFirstFileW)(LPCWSTR, LPWIN32_FIND_DATAW);
static HANDLE WINAPI FruityFindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    HANDLE r = orig_FindFirstFileW(path, data);
    FruityDbg("findfirst %ls -> %p (err %lu)\n", path ? path : L"<null>", r, GetLastError());
    return r;
}
static DWORD(WINAPI* orig_GetFileAttributesW)(LPCWSTR);
static DWORD WINAPI FruityGetFileAttributesW(LPCWSTR path) {
    DWORD r = orig_GetFileAttributesW(path);
    FruityDbg("getattr %ls -> %08lx (err %lu)\n", path ? path : L"<null>", (unsigned long)r, GetLastError());
    return r;
}
static DWORD(WINAPI* orig_ExpandEnvironmentStringsW)(LPCWSTR, LPWSTR, DWORD);
static DWORD WINAPI FruityExpandEnvironmentStringsW(LPCWSTR src, LPWSTR dst, DWORD size) {
    DWORD r = orig_ExpandEnvironmentStringsW(src, dst, size);
    FruityDbg("expandenv '%ls' -> '%ls' (r=%lu)\n", src ? src : L"<null>", (r && dst) ? dst : L"?", (unsigned long)r);
    return r;
}
static void HookFruityModule(HMODULE mod);
// A module a plug-in pulls in during construction (its *_Engine_x64.dll, IL
// companion libs) needs the IAT shims before any of its code runs — e.g.
// FLEX's Engine probes an optional oggio DLL and MessageBoxes the failure
// during create. Image-Line-tree modules only, never system DLLs.
static void HookNewIlModule(HMODULE r) {
    if (!r) return;
    wchar_t mp[MAX_PATH];
    if (!GetModuleFileNameW(r, mp, MAX_PATH)) return;
    if (!wcsstr(mp, L"\\Image-Line\\") && !wcsstr(mp, L"\\FruityMirror\\")) return;
    __try { HookFruityModule(r); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static HMODULE(WINAPI* orig_LoadLibraryW)(LPCWSTR);
static HMODULE(WINAPI* orig_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
static HMODULE IlSharedLoad(LPCWSTR path);
static HMODULE WINAPI FruityLoadLibraryW(LPCWSTR path) {
    HMODULE r = orig_LoadLibraryW(path);
    FruityDbg("loadlib %ls -> %p (err %lu)\n", path ? path : L"<null>", r, GetLastError());
    if (!r) r = IlSharedLoad(path);
    HookNewIlModule(r);
    return r;
}
// Shared support DLLs (oggio_x64.dll, ...) live in "<FL root>\Shared" — FL's
// dll dir, which plug-ins reach through search paths we don't replicate.
// When a bare-name load fails, retry it against the real Shared folder.
static HMODULE IlSharedLoad(LPCWSTR path) {
    const wchar_t* base = gFlInstallSharedW;
    if (!path || !base[0]) return nullptr;
    if (wcschr(path, L'\\') && wcsstr(path, L"\\Shared\\")) return nullptr;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    wchar_t full[MAX_PATH];
    swprintf_s(full, L"%s\\%s", base, name);
    return orig_LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}
static HMODULE WINAPI FruityLoadLibraryExW(LPCWSTR path, HANDLE f, DWORD flags) {
    HMODULE r = orig_LoadLibraryExW(path, f, flags);
    FruityDbg("loadlibex %ls -> %p (err %lu)\n", path ? path : L"<null>", r, GetLastError());
    if (!r) r = IlSharedLoad(path);
    HookNewIlModule(r);
    return r;
}
// Registry probing is log-only: shows which licence keys a plug-in consults.
static BOOL(WINAPI* orig_SetCurrentDirectoryW)(LPCWSTR);
static BOOL WINAPI FruitySetCurrentDirectoryW(LPCWSTR path) {
    FruityDbg("setcwd %ls\n", path ? path : L"<null>");
    return orig_SetCurrentDirectoryW(path);
}
static BOOL(WINAPI* orig_SetCurrentDirectoryA)(LPCSTR);
static BOOL WINAPI FruitySetCurrentDirectoryA(LPCSTR path) {
    FruityDbg("setcwd %s\n", path ? path : "<null>");
    return orig_SetCurrentDirectoryA(path);
}
static LSTATUS(WINAPI* orig_RegOpenKeyExW)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
static LSTATUS WINAPI FruityRegOpenKeyExW(HKEY key, LPCWSTR sub, DWORD opt, REGSAM sam, PHKEY out) {
    LSTATUS r = orig_RegOpenKeyExW(key, sub, opt, sam, out);
    FruityDbg("regopen %ls -> %ld\n", sub ? sub : L"<null>", (long)r);
    return r;
}
static LSTATUS(WINAPI* orig_RegQueryValueExW)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
static LSTATUS WINAPI FruityRegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD size) {
    LSTATUS r = orig_RegQueryValueExW(key, name, res, type, data, size);
    FruityDbg("regquery %ls -> %ld\n", name ? name : L"<null>", (long)r);
    return r;
}

// Patch mod's IAT entry for dll!fn to `hook`, saving the previous target in
// `orig`. Everything is bounds-checked against the image: a malformed import
// table must not let us write the hook address into arbitrary memory.
static void PatchIatSlot(HMODULE mod, const char* dll, const char* fn, void* hook, void** orig) {
    auto* dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = (IMAGE_NT_HEADERS64*)((char*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    DWORD imgSize = nt->OptionalHeader.SizeOfImage;
    DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    DWORD impSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (!impRva || impRva >= imgSize) return;
    auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)((char*)mod + impRva);
    int maxDesc = (int)std::min<DWORD>(impSize / sizeof(IMAGE_IMPORT_DESCRIPTOR) + 8, 1024);
    for (int d = 0; d < maxDesc && imp->Name; ++d, ++imp) {
        if (imp->Name >= imgSize) break;
        const char* dn = (const char*)mod + imp->Name;
        if (_stricmp(dn, dll)) continue;
        if (!imp->FirstThunk || imp->FirstThunk >= imgSize) continue;
        auto* iat = (IMAGE_THUNK_DATA64*)((char*)mod + imp->FirstThunk);
        auto* th = iat;
        if (imp->OriginalFirstThunk && imp->OriginalFirstThunk < imgSize)
            th = (IMAGE_THUNK_DATA64*)((char*)mod + imp->OriginalFirstThunk);
        for (int j = 0; j < 100000 && th->u1.AddressOfData; ++j, ++th, ++iat) {
            if (th->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            if (th->u1.AddressOfData >= imgSize || (uintptr_t)iat < (uintptr_t)mod) break;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)((char*)mod + th->u1.AddressOfData);
            if (strcmp(ibn->Name, fn)) continue;
            if ((void*)iat->u1.Function == hook) break;   // already patched
            DWORD oldp;
            VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &oldp);
            if (orig && *orig != (void*)iat->u1.Function)
                *orig = (void*)iat->u1.Function;   // keep the true original
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), oldp, &oldp);
            gIatHookCount++;
            break;
        }
    }
}

static void HookFruityModule(HMODULE mod) {
    PatchIatSlot(mod, "user32.dll", "DestroyWindow", (void*)FruityDestroyWindow, (void**)&orig_DestroyWindow);
    PatchIatSlot(mod, "user32.dll", "NtUserDestroyWindow", (void*)FruityNtUserDestroyWindow, (void**)&orig_NtUserDestroyWindow);
    PatchIatSlot(mod, "win32u.dll", "NtUserDestroyWindow", (void*)FruityNtUserDestroyWindow, (void**)&orig_NtUserDestroyWindow);
    PatchIatSlot(mod, "user32.dll", "PostMessageW", (void*)FruityPostMessageW, (void**)&orig_PostMessageW);
    PatchIatSlot(mod, "user32.dll", "MessageBoxW", (void*)FruityMessageBoxW, (void**)&orig_MessageBoxW);
    PatchIatSlot(mod, "user32.dll", "MessageBoxA", (void*)FruityMessageBoxA, (void**)&orig_MessageBoxA);
    PatchIatSlot(mod, "user32.dll", "MessageBoxExW", (void*)FruityMessageBoxExW, (void**)&orig_MessageBoxExW);
    PatchIatSlot(mod, "kernel32.dll", "CreateFileW", (void*)FruityCreateFileW, (void**)&orig_CreateFileW);
    PatchIatSlot(mod, "kernelbase.dll", "CreateFileW", (void*)FruityCreateFileW, (void**)&orig_CreateFileW);
    PatchIatSlot(mod, "kernel32.dll", "FindFirstFileW", (void*)FruityFindFirstFileW, (void**)&orig_FindFirstFileW);
    PatchIatSlot(mod, "kernelbase.dll", "FindFirstFileW", (void*)FruityFindFirstFileW, (void**)&orig_FindFirstFileW);
    PatchIatSlot(mod, "kernel32.dll", "GetFileAttributesW", (void*)FruityGetFileAttributesW, (void**)&orig_GetFileAttributesW);
    PatchIatSlot(mod, "kernelbase.dll", "GetFileAttributesW", (void*)FruityGetFileAttributesW, (void**)&orig_GetFileAttributesW);
    PatchIatSlot(mod, "kernel32.dll", "SetCurrentDirectoryW", (void*)FruitySetCurrentDirectoryW, (void**)&orig_SetCurrentDirectoryW);
    PatchIatSlot(mod, "kernelbase.dll", "SetCurrentDirectoryW", (void*)FruitySetCurrentDirectoryW, (void**)&orig_SetCurrentDirectoryW);
    PatchIatSlot(mod, "kernel32.dll", "SetCurrentDirectoryA", (void*)FruitySetCurrentDirectoryA, (void**)&orig_SetCurrentDirectoryA);
    PatchIatSlot(mod, "kernelbase.dll", "SetCurrentDirectoryA", (void*)FruitySetCurrentDirectoryA, (void**)&orig_SetCurrentDirectoryA);
    PatchIatSlot(mod, "kernel32.dll", "ExpandEnvironmentStringsW", (void*)FruityExpandEnvironmentStringsW, (void**)&orig_ExpandEnvironmentStringsW);
    PatchIatSlot(mod, "kernelbase.dll", "ExpandEnvironmentStringsW", (void*)FruityExpandEnvironmentStringsW, (void**)&orig_ExpandEnvironmentStringsW);
    PatchIatSlot(mod, "kernel32.dll", "LoadLibraryW", (void*)FruityLoadLibraryW, (void**)&orig_LoadLibraryW);
    PatchIatSlot(mod, "kernel32.dll", "LoadLibraryExW", (void*)FruityLoadLibraryExW, (void**)&orig_LoadLibraryExW);
    PatchIatSlot(mod, "kernelbase.dll", "LoadLibraryExW", (void*)FruityLoadLibraryExW, (void**)&orig_LoadLibraryExW);
    PatchIatSlot(mod, "kernel32.dll", "VirtualFree", (void*)FruityVirtualFree, (void**)&orig_VirtualFree);
    PatchIatSlot(mod, "kernelbase.dll", "VirtualFree", (void*)FruityVirtualFree, (void**)&orig_VirtualFree);
    PatchIatSlot(mod, "ntdll.dll", "NtFreeVirtualMemory", (void*)FruityNtFreeVM, (void**)&orig_NtFreeVM);
    PatchIatSlot(mod, "advapi32.dll", "RegOpenKeyExW", (void*)FruityRegOpenKeyExW, (void**)&orig_RegOpenKeyExW);
    PatchIatSlot(mod, "advapi32.dll", "RegQueryValueExW", (void*)FruityRegQueryValueExW, (void**)&orig_RegQueryValueExW);
    PatchIatSlot(mod, "kernel32.dll", "RegOpenKeyExW", (void*)FruityRegOpenKeyExW, (void**)&orig_RegOpenKeyExW);
    PatchIatSlot(mod, "kernel32.dll", "RegQueryValueExW", (void*)FruityRegQueryValueExW, (void**)&orig_RegQueryValueExW);
}
// Only the plug-in and the Image-Line support DLLs are patched. Never system or
// WPF modules: process-wide shims (e.g. a DestroyWindow that always succeeds)
// broke OLE drag-and-drop in the host UI.
static void HookIlModules(HMODULE plugin) {
    const char* names[] = {"QuickFontCache_x64.dll", "ILTools_x64.dll", "FruityPlug_x64.dll"};
    __try { HookFruityModule(plugin); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    for (auto* n : names) {
        HMODULE m = GetModuleHandleA(n);
        if (m) __try { HookFruityModule(m); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// FL's program root = the directory containing Plugins\Fruity. FHD_GetProgPath
// answers with it and plug-ins resolve companion files (engine DLLs, artwork)
// relative to it. Plug-ins outside an FL install fall back to three levels up.
static std::wstring FruityFlRoot(const std::wstring& pluginPath) {
    for (size_t i = 0; i + 16 <= pluginPath.size(); ++i)
        if (_wcsnicmp(pluginPath.c_str() + i, L"\\plugins\\fruity\\", 16) == 0 ||
            _wcsnicmp(pluginPath.c_str() + i, L"/plugins/fruity/", 16) == 0)
            return pluginPath.substr(0, i);
    std::wstring root = pluginPath;
    for (int i = 0; i < 3; ++i) root = root.substr(0, root.find_last_of(L"\\/"));
    return root;
}

// Plug-ins resolve resources relative to GetProgPath and also write there:
// Newtone drops dragged-out samples at the root and creates its data dir at
// <prog>\..\Shared\Plugins\Newtone. The real FL root lives under Program Files
// and is read-only for normal users, so we hand out a junction mirror:
// directory junctions make every read resolve into the real install while the
// mirror itself — plus the Shared\Plugins dirs and the plug-in's own path —
// stays writable. NTFS junctions need no admin rights (unlike symlinks).
static void CreateDirDeep(std::wstring path) {
    for (size_t i = 3; i <= path.size(); ++i)
        if (i == path.size() || path[i] == L'\\') {
            std::wstring part = path.substr(0, i);
            CreateDirectoryW(part.c_str(), nullptr);
        }
}
static bool CreateJunctionW(const std::wstring& link, const std::wstring& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            FruityDbg("junction: mkdir '%ls' err=%lu\n", link.c_str(), GetLastError());
            return false;
        }
        DWORD a = GetFileAttributesW(link.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) return true;   // already a junction
        // A plain existing dir gets the reparse point only if it is empty;
        // otherwise it stays a real dir (a plug-in may have written into it).
    }
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        FruityDbg("junction: open '%ls' err=%lu\n", link.c_str(), GetLastError());
        return false;
    }
    std::wstring subst = L"\\??\\" + target;
    for (auto& c : subst) if (c == L'/') c = L'\\';
    if (subst.size() + 1 + target.size() >= 1024) return false;
    const USHORT substLen = (USHORT)(subst.size() * sizeof(wchar_t));
    const USHORT printLen = (USHORT)(target.size() * sizeof(wchar_t));
    // Mount-point reparse buffer (stable ABI; the SDK's REPARSE_DATA_BUFFER
    // may be version-gated away depending on _WIN32_WINNT).
    struct {
        uint32_t tag; uint16_t dataLen; uint16_t reserved;
        uint16_t substOff, substLen, printOff, printLen;
        wchar_t  paths[1024];
    } rd{};
    rd.tag = 0xA0000003u;                            // IO_REPARSE_TAG_MOUNT_POINT
    rd.dataLen = (USHORT)(substLen + printLen + 12);
    rd.substLen = substLen;
    rd.printOff = substLen + 2;
    rd.printLen = printLen;
    memcpy(rd.paths, subst.c_str(), substLen);
    memcpy(rd.paths + subst.size() + 1, target.c_str(), printLen);
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, 0x000900A4 /*FSCTL_SET_REPARSE_POINT*/, &rd,
                              8 + rd.dataLen, nullptr, 0, &ret, nullptr) != 0;
    if (!ok) FruityDbg("junction: ioctl '%ls'->'%ls' err=%lu\n", link.c_str(), target.c_str(), GetLastError());
    CloseHandle(h);
    return ok;
}
// Recreate realDir's children inside mirrorDir as junctions (dirs) or hard
// links (files). skipName (case-insensitive) is left for the caller to mirror
// deeper — it must become a real directory, not a junction.
static void MirrorChildren(const std::wstring& realDir, const std::wstring& mirrorDir,
                           const std::wstring& skipName) {
    CreateDirDeep(mirrorDir);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((realDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!skipName.empty() && !_wcsicmp(fd.cFileName, skipName.c_str())) continue;
        std::wstring dst = mirrorDir + L"\\" + fd.cFileName;
        DWORD a = GetFileAttributesW(dst.c_str());
        if (a != INVALID_FILE_ATTRIBUTES &&
            (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        std::wstring src = realDir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            CreateJunctionW(dst, src);
        else
            CreateHardLinkW(dst.c_str(), src.c_str(), nullptr);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
// Builds (idempotently) the writable mirror of flRoot and returns the path to
// give plug-ins as their program path. pluginPath is used to keep the
// plug-in's own directory real so it can write beside its DLL.
static std::wstring EnsureFlMirror(const std::wstring& flRoot, const std::wstring& pluginPath) {
    wchar_t lad[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) return flRoot;
    uint32_t hash = 2166136261u;                     // FNV-1a, lowercased
    for (wchar_t c : flRoot) {
        if (c >= L'A' && c <= L'Z') c += 32;
        hash ^= (uint32_t)c; hash *= 16777619u;
    }
    wchar_t hex[9]; swprintf_s(hex, L"%08x", hash);
    std::wstring base = std::wstring(lad) + L"\\Phonic\\FruityMirror\\" + hex;

    // <base>\Shared answers "<prog>\..\Shared\<x>" lookups: mirror the real
    // sibling Shared (Image-Line\Shared) and always offer a writable Plugins.
    std::wstring parent = flRoot.substr(0, flRoot.find_last_of(L"\\/"));
    std::wstring shared = base + L"\\Shared";
    MirrorChildren(parent + L"\\Shared", shared, L"");
    CreateDirDeep(shared + L"\\Plugins");

    std::wstring root = base + L"\\Root";
    CreateDirDeep(root);
    // Split pluginPath relative to flRoot so the plug-in's own dir stays real.
    std::vector<std::wstring> rel;
    if (pluginPath.size() > flRoot.size() + 1 &&
        !_wcsnicmp(pluginPath.c_str(), flRoot.c_str(), flRoot.size()))
        for (size_t p = flRoot.size() + 1, n; p <= pluginPath.size(); p = n + 1) {
            n = pluginPath.find(L'\\', p);
            if (n == std::wstring::npos) n = pluginPath.size();
            rel.push_back(pluginPath.substr(p, n - p));
        }
    if (!rel.empty()) rel.pop_back();                // last component is the file
    // Mirror the real FL root one level: "Shared" gets the real-dir treatment
    // (writable Plugins inside); the plug-in's path head is mirrored deeper.
    std::wstring head = rel.empty() ? L"" : rel[0];
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW((flRoot + L"\\*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (!head.empty() && !_wcsicmp(fd.cFileName, head.c_str())) continue;
            std::wstring dst = root + L"\\" + fd.cFileName;
            DWORD a = GetFileAttributesW(dst.c_str());
            if (a != INVALID_FILE_ATTRIBUTES &&
                (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))) continue;
            std::wstring src = flRoot + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!_wcsicmp(fd.cFileName, L"Shared")) {
                    MirrorChildren(src, dst, L"");
                    CreateDirDeep(dst + L"\\Plugins");
                } else CreateJunctionW(dst, src);
            } else CreateHardLinkW(dst.c_str(), src.c_str(), nullptr);
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    // Real directories all the way down to the plug-in's own folder.
    std::wstring real = flRoot, mir = root;
    for (size_t i = 0; i < rel.size(); ++i) {
        real += L"\\" + rel[i]; mir += L"\\" + rel[i];
        MirrorChildren(real, mir, i + 1 < rel.size() ? rel[i + 1] : L"");
    }
    FruityDbg("mirror: '%ls' -> '%ls'\n", flRoot.c_str(), root.c_str());
    return root;
}

// FL ships private fonts (ILGlyphs, ILCursors, ILLogos, OpenSans, ...) under
// <FL>\Shared\Artwork\Fonts. Register them process-private so the plug-in's
// QuickFont lookups resolve to real faces.
static void LoadFlPrivateFonts(const std::wstring& flRoot) {
    std::wstring dir = flRoot + L"\\Shared\\Artwork\\Fonts\\";
    for (auto* pat : { L"*.ilfont", L"*.ttf", L"*.otf" }) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                AddFontResourceExW((dir + fd.cFileName).c_str(), FR_PRIVATE, nullptr);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// QuickFontCache exports manually-refcounted shared singletons; the plug-in's
// error paths release them once too often, leaving a dangling global. Extra
// acquires keep them alive, and gFruityPinRegion shields the heap segment.
static void* gQfcPins[16];
static int gQfcPinCount;
static void FruityDbg(const char* fmt, ...) {
    wchar_t wpath[MAX_PATH];
    if (!GetEnvironmentVariableW(L"MP_FRUITY_DEBUGLOG", wpath, MAX_PATH)) return;
    FILE* f = nullptr; if (_wfopen_s(&f, wpath, L"a") || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}

static void PinQfcSingletons() {
    __try {
        if (gQfcPinCount) return;
        HMODULE qfc = GetModuleHandleA("QuickFontCache_x64.dll");
        if (!qfc) { FruityDbg("pin: QuickFontCache not loaded\n"); return; }
        auto acquireBank = (void* (*)())GetProcAddress(qfc, "AcquireSharedThreadBank");
        auto acquire = (void* (*)())GetProcAddress(qfc, "AcquireSharedAnimTimer");
        if (acquireBank) gQfcPins[gQfcPinCount++] = acquireBank();
        if (acquire) {
            for (int i = 0; i < 8; ++i)   // survive repeat releases
                gQfcPins[gQfcPinCount++] = acquire();
            gFruityPinRegion = gQfcPins[gQfcPinCount - 1];
        }
        FruityDbg("pin: bank=%p timer=%p pinregion=%p\n",
                  gQfcPinCount > 0 ? gQfcPins[0] : nullptr,
                  gQfcPinCount > 1 ? gQfcPins[1] : nullptr, gFruityPinRegion);
    } __except (EXCEPTION_EXECUTE_HANDLER) { FruityDbg("pin: EXCEPTION during acquire\n"); }
}

static std::mutex gFruityCompatMutex;
static bool gFruityHooksInstalled;
static HMODULE gFruityPluginMod;
// After LoadLibraryW of the plug-in, before CreatePlugInstance.
static void InstallFruityHooks(HMODULE pluginMod, const std::wstring& pluginPath) {
    std::lock_guard lk(gFruityCompatMutex);
    if (gFruityHooksInstalled) return;
    gFruityPluginMod = pluginMod;
    if (!GetEnvironmentVariableW(L"MP_FRUITY_NOIAT", nullptr, 0))
        HookIlModules(pluginMod);
    std::wstring root = FruityFlRoot(pluginPath);
    if (!GetEnvironmentVariableW(L"MP_FRUITY_NOFONT", nullptr, 0))
        LoadFlPrivateFonts(root);   // no-op for plug-ins outside an FL install
    gFruityHooksInstalled = true;
}
// Before showing the editor (on the UI thread, where the VCL form will live —
// the Delphi/QFC calls are only safe there). Pins QFC's shared singletons and
// catches modules delay-loaded since Load().
static void InstallFruityPins(HMODULE pluginMod) {
    std::lock_guard lk(gFruityCompatMutex);
    if (!GetEnvironmentVariableW(L"MP_FRUITY_NOPIN", nullptr, 0))
        PinQfcSingletons();
    if (!GetEnvironmentVariableW(L"MP_FRUITY_NOIAT", nullptr, 0) && pluginMod)
        HookIlModules(pluginMod);
}

void BuildHostObject(FruityHost& h, const char* progPath, Node* owner) {
    static HostVTable vt = [] {
        HostVTable v;
        for (auto& f : v.f) f = (void*)HostStub;
        v.f[0] = (void*)HostDispatcher;
        v.f[1] = (void*)HostOnParamChanged;
        v.f[16] = (void*)HostSongMixingTime;   // GetSongMixingTime (int ticks)
        v.f[17] = (void*)HostSongTimeExact;    // GetSongMixingTime_A
        v.f[18] = (void*)HostSongTimeExact;    // GetSongPlayingTime
        v.f[36] = (void*)HostGetBuffer;
        v.f[37] = (void*)HostGetBuffer;
        for (auto& f : serviceVt) f = (void*)ServiceStub;
        if (gFruityDbgOn)   // per-slot stubs identify which service method is called
            [&]<size_t... I>(std::index_sequence<I...>) {
                ((serviceVt[I] = (void*)&ServiceStubN<(int)I>), ...);
            }(std::make_index_sequence<96>{});
        serviceVt[0] = (void*)ServiceLookup;
        serviceVt[1] = (void*)ServiceRef;
        serviceVt[2] = (void*)ServiceRef;
        for (auto& f : resourceVt) f = (void*)ServiceStub;
        resourceVt[0] = (void*)ResourceLookup;
        for (auto& f : mgrInnerVt) f = (void*)ServiceStub;
        mgrInnerVt[0] = (void*)MgrInnerQuery;
        for (auto& f : mgrSvcVt) f = (void*)ServiceStub;
        mgrSvcVt[0] = (void*)ServiceLookup;
        mgrSvcVt[1] = (void*)ServiceRef;
        mgrSvcVt[2] = (void*)MgrOp;
        mgrSvcVt[3] = (void*)MgrIfQuery;
        mgrSvcVt[6] = (void*)MgrCreate;
        return v;
    }();
    h = {};
    h.vftable = vt.f;
    h.HostVersion = 25000000;
    // Shared host wavetables (16384-sample sine/tri/sqr/saw/saw/noise).
    static float tables[10][16384];
    static bool filled = [] {
        for (int w = 0; w < 10; ++w)
            for (int i = 0; i < 16384; ++i) {
                float t = (float)i / 16384.f;
                switch (w) {
                case 0: tables[w][i] = sinf(t * 6.2831853f); break;
                case 1: tables[w][i] = t < 0.5f ? 4.f * t - 1.f : 3.f - 4.f * t; break;
                case 2: tables[w][i] = t < 0.5f ? 1.f : -1.f; break;
                case 3: case 4: tables[w][i] = 2.f * t - 1.f; break;
                case 5: tables[w][i] = ((i * 1103515245u + 12345u) >> 16) / 32768.f - 1.f; break;
                }
            }
        return true;
    }();
    (void)filled;
    for (int w = 0; w < 10; ++w) h.WaveTables[w] = tables[w];
    static float temp[4][8192 * 2];
    for (int i = 0; i < 4; ++i) h.TempBuffers[i] = temp[i];
    h.Owner = owner;
    h.ProgPath = progPath;
    gAnyHost = &h;
}

// SEH-guarded entry points into the plug-in DLL.
// Delphi exception object: raw dump, class name (vmtClassName PShortString)
// and UnicodeString message fields. No destructible locals — __try is legal.
static void DumpDelphiException(void** obj) {
    __try {
        FruityDbg("  exc obj:");
        for (int i = 0; i < 8; ++i) FruityDbg(" %p", obj[i]);
        FruityDbg("\n");
        void** vmt = (void**)obj[0];
        for (int i = -2; i >= -10; --i) __try {
            auto* nm = (unsigned char*)vmt[i];
            if (nm && nm[0] > 0 && nm[0] < 64) {
                bool ok2 = true;
                for (int j = 1; j <= nm[0]; ++j) if (nm[j] < 32 || nm[j] > 126) { ok2 = false; break; }
                if (ok2) { FruityDbg("  class: %.*s\n", (int)nm[0], nm + 1); break; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        for (int i = 1; i < 8; ++i) __try {
            auto* s = (char*)obj[i];
            if (!s) continue;
            MEMORY_BASIC_INFORMATION mbi = {};
            if (!VirtualQuery(s, &mbi, sizeof mbi) || !(mbi.State & MEM_COMMIT)) continue;
            char a[33] = {}; for (int j = 0; j < 32; ++j) a[j] = s[j];
            wchar_t w[17] = {}; for (int j = 0; j < 16; ++j) w[j] = ((wchar_t*)s)[j];
            FruityDbg("    field[%d]=%p ansi='%s' wide='%ls'\n", i, s, a, w);
            int len = *(int*)(s - 4);
            if (len > 0 && len < 400) {
                char t[64] = {};
                for (int j = 0; j < len && j < 60; ++j) { auto c = ((wchar_t*)s)[j]; t[j] = c >= 32 && c < 127 ? (char)c : '?'; }
                FruityDbg("      ustr len=%d text='%s'\n", len, t);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// First-chance exception log for the create window (debug only): the Delphi
// RTL wraps faults in EEDFADE and shows "Runtime error N at XXXX" boxes, so
// the raw exception record is the only place the real faulting RIP survives.
static LONG CALLBACK FruityVeh(EXCEPTION_POINTERS* ep) {
    auto* r = ep->ExceptionRecord;
    if (r->ExceptionCode == 0xEEDFADE) {
        // Walk the raw stack for module-space return addresses: the raise
        // chain shows which plug-in code threw, without symbols.
        static bool dumped;
        if (!dumped) {
            dumped = true;
            FruityDbg("eedfade stack (rip=%p):\n", (void*)ep->ContextRecord->Rip);
            __try {
                auto* sp = (void**)ep->ContextRecord->Rsp;
                for (int i = 0; i < 96; ++i) {
                    HMODULE hm = nullptr;
                    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            (LPCWSTR)sp[i], &hm) || !hm) continue;
                    wchar_t mn[MAX_PATH] = {};
                    GetModuleFileNameW(hm, mn, MAX_PATH);
                    const wchar_t* b = wcsrchr(mn, L'\\');
                    FruityDbg("  rsp+%03x: %ls+0x%llx\n", i * 8, b ? b + 1 : mn,
                              (unsigned long long)((char*)sp[i] - (char*)hm));
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HMODULE hm = nullptr;
    if (r->ExceptionAddress &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)r->ExceptionAddress, &hm)) {
        wchar_t mn[MAX_PATH] = {};
        GetModuleFileNameW(hm, mn, MAX_PATH);
        const wchar_t* b = wcsrchr(mn, L'\\');
        FruityDbg("veh: %08lx @%ls+0x%llx (r/w=%llx)\n", r->ExceptionCode, b ? b + 1 : mn,
                  (unsigned long long)((char*)r->ExceptionAddress - (char*)hm),
                  r->NumberParameters > 1 ? (unsigned long long)r->ExceptionInformation[1] : 0ull);
    } else {
        FruityDbg("veh: %08lx @%p (nomod, r/w=%llx)\n", r->ExceptionCode, r->ExceptionAddress,
                  r->NumberParameters > 1 ? (unsigned long long)r->ExceptionInformation[1] : 0ull);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Second-attempt handler only (installed after a first CreatePlugInstance
// dies to EEDFADE): some plug-ins raise a Delphi exception for a non-fatal
// internal failure — Newtone cannot create its data dir under Program
// Files\Image-Line\Shared\Plugins outside FL, raises, yet finishes a fully
// working instance when the raise is skipped (the standalone probe does
// exactly this). Continuing past RaiseException resumes the plug-in code
// after the raise statement.
static LONG CALLBACK FruitySwallowVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xEEDFADE) {
        FruityDbg("create: swallowed Delphi raise at %p\n",
                  ep->ExceptionRecord->ExceptionAddress);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static unsigned gLastCreateExCode;
static void* gLastCreateExAddr;
static ULONG_PTR gLastCreateExInfo[8];
void* GuardedCreate(void* fn, void* host, IntP tag, bool& ok) {
    __try { return ((void*(__stdcall*)(void*, IntP))fn)(host, tag); }
    __except (gLastCreateExCode = GetExceptionCode(),
              gLastCreateExAddr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
              [](EXCEPTION_POINTERS* ep) {
                  auto* r = ep->ExceptionRecord;
                  DWORD n = r->NumberParameters < 8 ? r->NumberParameters : 8;
                  for (DWORD i = 0; i < n; ++i) gLastCreateExInfo[i] = r->ExceptionInformation[i];
                  return EXCEPTION_EXECUTE_HANDLER;
              }(GetExceptionInformation())) { ok = false; return nullptr; }
}
IntP GuardedPlugCall(void* plug, int slot, IntP a, IntP b, IntP c, bool& ok) {
    __try {
        auto fn = (IntP(__stdcall*)(void*, IntP, IntP, IntP))(*(void***)plug)[slot];
        return fn(plug, a, b, c);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; return 0; }
}
void GuardedGetName(void* plug, int section, int index, int value, char* out, bool& ok) {
    __try {
        auto fn = (void(__stdcall*)(void*, int, int, int, char*))(*(void***)plug)[PV_GetName];
        fn(plug, section, index, value, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
}
bool GuardedRender(void* plug, const float* in, float* out, int frames) {
    __try {
        auto fn = (void(__stdcall*)(void*, const float*, float*, int))(*(void***)plug)[PV_EffRender];
        fn(plug, in, out, frames);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Generators advance their voice engine on per-tick host calls, separate
// from Gen_Render — voices that never see a tick stay silent.
void GuardedNoArgCall(void* plug, int slot) {
    __try {
        auto fn = (void(__stdcall*)(void*))(*(void***)plug)[slot];
        fn(plug);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Gen_Render(DestBuffer, var Length): generators may render fewer samples;
// len returns the produced count.
bool GuardedGenRender(void* plug, float* dest, int& len) {
    __try {
        auto fn = (void(__stdcall*)(void*, float*, int*))(*(void***)plug)[PV_GenRender];
        fn(plug, dest, &len);
        return len > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// MIDIIn(var Msg): packed TMIDIOutMsg {Status,Data1,Data2,Port} by reference —
// the plug-in may rewrite it (MIDIMsg_Null to kill).
void GuardedMIDIIn(void* plug, uint32_t msg) {
    int m = (int)msg;
    __try {
        auto fn = (void(__stdcall*)(void*, int*))(*(void***)plug)[PV_MIDIIn];
        fn(plug, &m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Piano-roll notes reach generators through the voice API, not MIDIIn:
// TriggerVoice(TVoiceParams*, SetTag) -> handle, Voice_Release/Kill(handle).
struct VoiceLevels { float pan, vol, pitch, fcut, fres; };   // TLevelParams (new)
struct VoiceParams { VoiceLevels init, fin; };               // TVoiceParams
IntP GuardedTriggerVoice(void* plug, VoiceParams* vp, IntP tag, bool& ok) {
    __try {
        auto fn = (IntP(__stdcall*)(void*, VoiceParams*, IntP))(*(void***)plug)[PV_TriggerVoice];
        return fn(plug, vp, tag);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; return 0; }
}
void GuardedVoiceCall(void* plug, int slot, IntP handle) {
    __try {
        auto fn = (void(__stdcall*)(void*, IntP))(*(void***)plug)[slot];
        fn(plug, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void GuardedSaveRestore(void* plug, IStream* stream, int save, bool& ok) {
    __try { GuardedPlugCall(plug, PV_SaveRestore, (IntP)stream, save, 0, ok); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
}

// Fruity DLLs are never unloaded (FL Studio doesn't either): the plug-ins and
// Image-Line's support libraries (QuickFontCache, ILTools) leave worker threads,
// timers and shared singletons running after the last instance is destroyed.
// FreeLibrary pulled their code out from under those threads, crashing the app
// as soon as e.g. Undo removed the last Fruity node.
struct FruityModule {
    HMODULE dll = nullptr;
    void* create = nullptr;
};
static std::atomic<bool> gAnyFruityLoaded{false};
std::map<std::wstring, std::weak_ptr<FruityModule>>& ModuleCache() {
    static std::map<std::wstring, std::weak_ptr<FruityModule>> cache;
    return cache;
}

class FruityNode final : public Node, public FruityGen {
public:
    explicit FruityNode(int id) : Node(id) {}
    ~FruityNode() override { CloseEditor(); Unload(); }

    const char* Type() const override { return "fruity"; }
    bool IsPlugin() const override { return true; }
    void Prepare(double sampleRate, int maxBlock) override;
    void Release() override {}
    void Process(mp::ProcessContext& ctx) override;

    int ParamCount() const override { return numParams; }
    bool GetParamInfo(int index, MP_ParamInfo& info) const override;
    bool GetParam(uint32_t pid, double& value) const override;
    bool SetParam(uint32_t pid, double value) override;
    std::wstring FormatParam(uint32_t pid, double value) const override;
    std::vector<uint8_t> GetState() const override;
    bool SetState(const uint8_t* data, size_t size) override;
    std::vector<std::wstring> PresetFiles() const override;
    bool LoadPresetFile(const std::wstring& path) override;
    int LatencySamples() const override { return (int)fhost.Latency; }
    bool HasEditor() const override { return plug != nullptr; }
    bool OpenEditor(void* owner, const wchar_t* title) override;
    void CloseEditor() override;
    bool EditorOpen() const override { return editorHwnd && IsWindow(editorHwnd); }
    void Idle() override;

    bool Load(const std::wstring& path, std::wstring& error);
    bool IsGenerator() const override { return isGenerator; }
    void SendMIDI(uint32_t msg) override;
    intptr_t NoteOn(int semitone, float velocity) override;
    void NoteOff(intptr_t voice) override;

private:
    void Unload();
    void SyncTransport(const mp::Transport& t);
    int RawParam(int index) const;              // current raw value
    int ToRaw(int index, double normalized) const;
    double ToNormalized(int index, int raw) const;

    std::shared_ptr<FruityModule> module;
    FruityHost fhost{};
    std::wstring flRoot;                            // real FL install root (pre-mirror)
    char progPath[MAX_PATH] = {};
    char projDataPath[MAX_PATH] = {};
    void* plug = nullptr;
    FruityInfo info{};
    bool isGenerator = false;
    int numParams = 0;
    std::vector<uint8_t> paramIsFloat;          // PI_Float per param
    std::vector<std::string> paramNames;
    std::vector<float> inBuf, outBuf;           // interleaved stereo scratch
    int maxBlock = 0;
    double rate = 48000;
    HWND editorHwnd = nullptr;
    FruityTiming timing;                        // read by host callbacks via fhost.Timing
    double sentTempo = 0;                       // audio thread: last tempo told to the plug-in
    bool sentPlaying = false;
};

bool FruityNode::Load(const std::wstring& path, std::wstring& error) {
    {
        auto& cache = ModuleCache();
        if (auto it = cache.find(path); it != cache.end()) {
            if (auto existing = it->second.lock()) module = existing;
        }
        if (!module) {
            auto m = std::make_shared<FruityModule>();
            m->dll = LoadLibraryW(path.c_str());
            if (!m->dll) { error = L"Could not load the DLL."; return false; }
            gAnyFruityLoaded = true;
            m->create = (void*)GetProcAddress(m->dll, "CreatePlugInstance");
            if (!m->create) { error = L"Not a Fruity plug-in (no CreatePlugInstance export)."; return false; }
            module = m;
            cache[path] = m;
        }
    }
    flRoot = FruityFlRoot(path);
    wcscpy_s(gFlRootW, flRoot.c_str());
    swprintf_s(gFlInstallSharedW, L"%s\\Shared", flRoot.c_str());
    std::wstring wprog = flRoot;
    if (!GetEnvironmentVariableW(L"MP_FRUITY_NOMIRROR", nullptr, 0))
        wprog = EnsureFlMirror(wprog, path);
    int plen = WideCharToMultiByte(CP_UTF8, 0, wprog.c_str(), -1, progPath, MAX_PATH, nullptr, nullptr);
    if (plen <= 0) progPath[0] = 0;
    // FL's GetProgPath ends in a backslash; plug-ins concatenate onto it raw.
    size_t ppl = strlen(progPath);
    if (ppl && progPath[ppl - 1] != '\\' && ppl + 1 < MAX_PATH) { progPath[ppl] = '\\'; progPath[ppl + 1] = 0; }
    BuildHostObject(fhost, progPath, this);
    fhost.Timing = &timing;
    fhost.IsPatcher = path.find(L"Patcher") != std::wstring::npos;
    PWSTR docs = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs) == S_OK) {
        std::wstring il = std::wstring(docs) + L"\\Image-Line\\";
        CoTaskMemFree(docs);
        if (GetFileAttributesW(il.c_str()) != INVALID_FILE_ATTRIBUTES)
            WideCharToMultiByte(CP_UTF8, 0, il.c_str(), -1, projDataPath, MAX_PATH, nullptr, nullptr);
    }
    if (projDataPath[0]) fhost.ProjData = projDataPath;
    if (GetEnvironmentVariableW(L"MP_FRUITY_NOCOMPAT", nullptr, 0) == 0) {
        HookIlModules(module->dll);   // this plug-in's IAT, always
        InstallFruityHooks(module->dll, path);   // sweep + private fonts, once per process
    }

    if (!GetEnvironmentVariableW(L"MP_FRUITY_NOCOM", nullptr, 0))
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = true;
    gCreatingHost = &fhost;
    void* veh = nullptr;
    if (GetEnvironmentVariableW(L"MP_FRUITY_DEBUGLOG", nullptr, 0))
        veh = AddVectoredExceptionHandler(1, FruityVeh);
    plug = GuardedCreate(module->create, &fhost, 0x7ea1, ok);   // FL's host tag
    if (!ok && gLastCreateExCode == 0xEEDFADE) {
        // Non-fatal Delphi raise during construction (e.g. Newtone's data-dir
        // creation): retry once with raises skipped. Plug-ins that construct
        // cleanly never see this path.
        FruityDbg("create: EEDFADE, retrying with raise-swallowing handler\n");
        ok = true;
        void* sw = AddVectoredExceptionHandler(1, FruitySwallowVeh);
        plug = GuardedCreate(module->create, &fhost, 0x7ea1, ok);
        RemoveVectoredExceptionHandler(sw);
    }
    if (veh) RemoveVectoredExceptionHandler(veh);
    gCreatingHost = nullptr;
    if (!ok) {
        FruityDbg("create exception: code=%08x addr=%p params=%016llx %016llx %016llx %016llx\n",
                  gLastCreateExCode, gLastCreateExAddr,
                  (unsigned long long)gLastCreateExInfo[0], (unsigned long long)gLastCreateExInfo[1],
                  (unsigned long long)gLastCreateExInfo[2], (unsigned long long)gLastCreateExInfo[3]);
        if (gLastCreateExCode == 0xEEDFADE && gLastCreateExInfo[1])
            DumpDelphiException((void**)gLastCreateExInfo[1]);
    }
    HMODULE vmod = nullptr;
    // A valid instance is an object whose vtable points into executable code.
    // Multi-DLL plug-ins (Newtone, Pitcher, ...) implement the object in a
    // companion *_Engine_x64.dll, so any loaded module counts, not just the
    // entry DLL itself.
    bool vtableOk = plug && *(void***)plug &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)* (void***)plug, &vmod);
    FruityDbg("create %ls: plug=%p ok=%d vtable=%p vmod=%p info=%p\n", path.c_str(),
              plug, (int)ok, plug ? *(void***)plug : nullptr, vmod,
              plug ? *(void**)((char*)plug + 16) : nullptr);
    if (!ok || !plug) {
        error = ok ? L"The plug-in did not initialize (constructor returned no object)."
                   : L"The plug-in did not initialize (constructor crashed).";
        plug = nullptr;
        return false;
    }
    if (!vtableOk) {
        error = L"The plug-in did not initialize (invalid object layout).";
        plug = nullptr;
        return false;
    }
    if (!ReadInfo(*(void**)((char*)plug + 16), info)) {
        error = L"The plug-in did not initialize (unsupported Fruity protocol version).";
        FruityDbg("info block unrecognized, first qwords: %016llx %016llx %016llx %016llx\n",
                  ((uint64_t*)*(void**)((char*)plug + 16))[0], ((uint64_t*)*(void**)((char*)plug + 16))[1],
                  ((uint64_t*)*(void**)((char*)plug + 16))[2], ((uint64_t*)*(void**)((char*)plug + 16))[3]);
        plug = nullptr;
        return false;
    }
    FruityDbg("info: '%s' flags=%08x params=%d outctrls=%d\n",
              info.longName ? info.longName : "?", info.flags, info.numParams, info.numOutCtrls);
    isGenerator = (info.flags & FPF_Generator) != 0;
    numParams = std::clamp(info.numParams, 0, 4096);
    paramIsFloat.assign(numParams, 0);
    paramNames.assign(numParams, {});
    for (int i = 0; i < numParams; ++i) {
        IntP flags = GuardedPlugCall(plug, PV_Dispatcher, FPD_GetParamInfo, i, 0, ok);
        if (ok && (flags & PI_Float)) paramIsFloat[i] = 1;
        char name[256] = {};
        GuardedGetName(plug, FPN_Param, i, 0, name, ok);
        paramNames[i] = name[0] ? name : ("Param " + std::to_string(i + 1));
    }
    inputs = {{2, L"In"}};
    outputs = {{2, L"Out"}};
    return true;
}

void FruityNode::Unload() {
    if (plug) {
        bool ok = true;
        FruityDbg("unload: DestroyObject\n");
        GuardedPlugCall(plug, PV_DestroyObject, 0, 0, 0, ok);
        FruityDbg("unload: DestroyObject ok=%d\n", ok ? 1 : 0);
        plug = nullptr;
    }
    FruityDbg("unload: release module\n");
    module = nullptr;
    FruityDbg("unload: done\n");
}

void FruityNode::Prepare(double sampleRate, int maxBlock_) {
    maxBlock = maxBlock_;
    rate = sampleRate;
    sentTempo = 0;
    sentPlaying = false;
    inBuf.assign(maxBlock * 2, 0.f);
    outBuf.assign(maxBlock * 2, 0.f);
    if (!plug) return;
    float spt = (float)(sampleRate / (120.0 / 60.0 * 96.0));   // 120 bpm, 96 ppq
    IntP sptBits = 0; memcpy(&sptBits, &spt, 4);
    TimeSigInfo sig{4, 4, 96};
    bool ok = true;
    struct { IntP id, val; } init[] = {
        {FPD_SetSampleRate, (IntP)(int)sampleRate}, {FPD_SetBlockSize, maxBlock},
        {FPD_SetNumSends, 4}, {FPD_SetTimeSig, (IntP)&sig}, {FPD_SetSamplesPerTick, sptBits},
        {FPD_ProcessMode, 0}, {FPD_RoutingChanged, 0}, {FPD_SetEnabled, 1}, {FPD_SetPlaying, 0},
    };
    for (auto& m : init) { GuardedPlugCall(plug, PV_Dispatcher, m.id, 0, m.val, ok); if (!ok) { failed = true; lastError = L"Plug-in rejected the processing setup."; return; } }
}

// Tells the plug-in about tempo changes and starts the (always playing)
// transport. These dispatcher IDs are documented as mixing-thread safe.
void FruityNode::SyncTransport(const mp::Transport& t) {
    timing.ticks.store(t.beats * kFruityPPQ, std::memory_order_relaxed);
    timing.tempo.store(t.tempo, std::memory_order_relaxed);
    bool ok = true;
    if (t.tempo != sentTempo) {
        sentTempo = t.tempo;
        const float bpm = (float)t.tempo, spt = (float)(rate * 60.0 / (t.tempo * kFruityPPQ));
        int bpmBits, sptBits;
        memcpy(&bpmBits, &bpm, 4); memcpy(&sptBits, &spt, 4);
        GuardedPlugCall(plug, PV_ProcessEvent, FPE_Tempo, bpmBits, (IntP)spt, ok);
        GuardedPlugCall(plug, PV_Dispatcher, FPD_SetSamplesPerTick, 0, sptBits, ok);
    }
    if (!sentPlaying) {
        sentPlaying = true;
        GuardedPlugCall(plug, PV_Dispatcher, FPD_SongPosChanged, 0, 0, ok);
        GuardedPlugCall(plug, PV_Dispatcher, FPD_SetPlaying, 0, 1, ok);
    }
}

void FruityNode::Process(mp::ProcessContext& ctx) {
    const int n = std::min(ctx.frames, maxBlock);
    SyncTransport(ctx.transport);
    const int inch = ctx.inChannels[0];
    int produced = n;
    bool ok;
    if (isGenerator) {
        GuardedNoArgCall(plug, PV_NewTick);
        GuardedNoArgCall(plug, PV_MIDITick);
        ok = GuardedGenRender(plug, outBuf.data(), produced);
    } else {
        float* src = inBuf.data();
        for (int i = 0; i < n; ++i) {
            src[i * 2] = inch > 0 ? ctx.in[0][0][i] : 0.f;
            src[i * 2 + 1] = inch > 1 ? ctx.in[0][1][i] : src[i * 2];
        }
        ok = GuardedRender(plug, src, outBuf.data(), n);
    }
    if (!ok) {
        failed = true;
        for (int c = 0; c < ctx.outChannels[0]; ++c) {
            const float* s = !isGenerator && inch > 0 ? ctx.in[0][std::min(c, inch - 1)] : nullptr;
            if (s) std::copy(s, s + n, ctx.out[0][c]); else std::fill(ctx.out[0][c], ctx.out[0][c] + n, 0.f);
        }
        return;
    }
    produced = std::clamp(produced, 0, n);
    const float* dst = outBuf.data();
    for (int c = 0; c < ctx.outChannels[0]; ++c) {
        for (int i = 0; i < produced; ++i) ctx.out[0][c][i] = dst[i * 2 + std::min(c, 1)];
        if (produced < n) std::fill(ctx.out[0][c] + produced, ctx.out[0][c] + n, 0.f);
    }
}

void FruityNode::SendMIDI(uint32_t msg) {
    if (!plug || failed) return;
    GuardedMIDIIn(plug, msg);
}
intptr_t FruityNode::NoteOn(int semitone, float velocity) {
    if (!plug || failed) return 0;
    VoiceParams vp{};
    vp.init = {0.f, std::clamp(velocity, 0.f, 1.f), semitone * 100.f, 0.f, 0.f};
    vp.fin = vp.init;
    bool ok = true;
    IntP h = GuardedTriggerVoice(plug, &vp, 0, ok);
    if (!ok || !h) return 0;
    FruityDbg("note: trigger %d vel=%.2f -> voice %lld\n", semitone, velocity, (long long)h);
    return (intptr_t)h;
}
void FruityNode::NoteOff(intptr_t voice) {
    if (!plug || failed || !voice) return;
    GuardedVoiceCall(plug, PV_VoiceRelease, (IntP)voice);
}

int FruityNode::RawParam(int index) const {
    bool ok = true;
    return (int)GuardedPlugCall(plug, PV_ProcessParam, index, 0, REC_GetValue, ok);
}
int FruityNode::ToRaw(int index, double normalized) const {
    normalized = std::clamp(normalized, 0.0, 1.0);
    if (paramIsFloat[index]) { float f = (float)normalized; int raw; memcpy(&raw, &f, 4); return raw; }
    return (int)std::lround(normalized * 65536.0);
}
double FruityNode::ToNormalized(int index, int raw) const {
    if (paramIsFloat[index]) { float f; memcpy(&f, &raw, 4); return std::isfinite(f) ? std::clamp((double)f, 0.0, 1.0) : 0.0; }
    return std::clamp(raw / 65536.0, 0.0, 1.0);
}

bool FruityNode::GetParamInfo(int index, MP_ParamInfo& out) const {
    if (index < 0 || index >= numParams) return false;
    out = {};
    out.id = (uint32_t)index;
    out.flags = MP_PARAM_AUTOMATABLE | MP_PARAM_NORMALIZED;
    out.minValue = 0; out.maxValue = 1;
    int raw = const_cast<FruityNode*>(this)->RawParam(index);
    out.defaultValue = ToNormalized(index, raw);
    MultiByteToWideChar(CP_UTF8, 0, paramNames[index].c_str(), -1, out.name, 128);
    return true;
}
bool FruityNode::GetParam(uint32_t pid, double& value) const {
    if (pid >= (uint32_t)numParams) return false;
    value = ToNormalized((int)pid, const_cast<FruityNode*>(this)->RawParam((int)pid));
    return true;
}
bool FruityNode::SetParam(uint32_t pid, double value) {
    if (pid >= (uint32_t)numParams || !std::isfinite(value)) return false;
    bool ok = true;
    GuardedPlugCall(plug, PV_ProcessParam, (IntP)pid, ToRaw((int)pid, value), REC_UpdateValue, ok);
    paramsChanged++;
    return ok;
}
std::wstring FruityNode::FormatParam(uint32_t pid, double value) const {
    if (pid >= (uint32_t)numParams) return L"";
    char text[256] = {};
    bool ok = true;
    GuardedGetName(plug, FPN_ParamValue, (int)pid, ToRaw((int)pid, value), text, ok);
    if (ok && text[0]) {
        wchar_t w[256];
        MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 256);
        return w;
    }
    return std::to_wstring(value);
}

std::vector<uint8_t> FruityNode::GetState() const {
    auto* stream = new MemStream();
    bool ok = true;
    GuardedSaveRestore(plug, stream, 1, ok);
    const auto& bytes = stream->Bytes();
    std::vector<uint8_t> out(8 + bytes.size());
    memcpy(out.data(), "MPFR", 4);
    uint32_t s = (uint32_t)bytes.size(); memcpy(out.data() + 4, &s, 4);
    if (s) memcpy(out.data() + 8, bytes.data(), s);
    stream->Release();
    return out;
}
bool FruityNode::SetState(const uint8_t* bytes, size_t size) {
    if (size < 8 || memcmp(bytes, "MPFR", 4) != 0) return false;
    uint32_t s = 0; memcpy(&s, bytes + 4, 4);
    if (8ull + s > size) return false;
    suspended = true;
    if (host) host->AudioBarrier();
    auto* stream = new MemStream();
    stream->Load(bytes + 8, s);
    bool ok = true;
    GuardedSaveRestore(plug, stream, 0, ok);
    stream->Release();
    suspended = false;
    paramsChanged++;
    return ok;
}

// FL hands preset files to the plug-in itself through FPD_LoadFile (filename
// in Value): Patcher & co parse .fst internally and rebuild their graph.
// Plug-ins that don't take files get the .fst state chunk (event 213) pushed
// through SaveRestoreState, followed by FPD_PluginLoaded like a real project
// load, since the chunk is the same SaveRestoreState byte stream.
std::vector<std::wstring> FruityNode::PresetFiles() const {
    if (!info.longName) return {};
    wchar_t name[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, info.longName, -1, name, 128);
    return FindFstPresets(name, FlPresetRoots(flRoot));
}
bool FruityNode::LoadPresetFile(const std::wstring& path) {
    if (!plug) { lastError = L"The plug-in is not loaded."; return false; }
    suspended = true;
    if (host) host->AudioBarrier();
    // Some plug-ins (Patcher) parse the .fst themselves and rebuild internally,
    // but still return 0 from FPD_LoadFile. Treat the load as accepted when the
    // plug-in's own serialized state changed — otherwise keep trying.
    auto snapState = [&] {
        auto* snap = new MemStream(); bool sok = true;
        GuardedSaveRestore(plug, snap, 1, sok);
        std::vector<uint8_t> out = snap->Bytes();
        snap->Release();
        return out;
    };
    char utf8path[2 * MAX_PATH];
    if (WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8path, sizeof(utf8path), nullptr, nullptr) > 0) {
        auto before = snapState();
        for (IntP type : { (IntP)1 /*LFT_Patcherize*/, (IntP)2 /*LFT_DroppedFile*/, (IntP)0 /*LFT_Generic*/ }) {
            bool ok = true;
            IntP r = GuardedPlugCall(plug, PV_Dispatcher, 18 /*FPD_LoadFile*/, type, (IntP)utf8path, ok);
            if (!ok) break;
            bool changed = snapState() != before;
            FruityDbg("preset: FPD_LoadFile(%lld) '%ls' -> r=%lld changed=%d\n",
                      (long long)type, path.c_str(), (long long)r, changed ? 1 : 0);
            if (r == 1 || changed) {
                bool ok2 = true;
                // FL follows a document load with project/plugin-loaded
                // notifications; Patcher rebuilds its surface + GUI tab on these.
                GuardedPlugCall(plug, PV_Dispatcher, 27 /*FPD_ProjLoaded*/, 0, 0, ok2);
                GuardedPlugCall(plug, PV_Dispatcher, 40 /*FPD_PluginLoaded*/, 0, 0, ok2);
                suspended = false; paramsChanged++;
                return true;
            }
        }
    }
    FstChunk fst;
    bool ok = ParseFstFile(path, fst) && !fst.data.empty();
    if (ok) {
        auto* stream = new MemStream();
        stream->Load(fst.data.data(), fst.data.size());
        GuardedSaveRestore(plug, stream, 0, ok);
        stream->Release();
        if (ok) {
            bool ok2 = true;
            GuardedPlugCall(plug, PV_Dispatcher, 40 /*FPD_PluginLoaded*/, 0, 0, ok2);
        }
    }
    suspended = false;
    paramsChanged++;
    FruityDbg("preset: chunk '%ls' (%zu bytes) -> %s\n", path.c_str(), fst.data.size(), ok ? "loaded" : "FAILED");
    if (!ok) lastError = L"The preset could not be loaded.";
    return ok;
}

// Editor: the plugin builds a Delphi VCL form (TWPControlForm) as a child of a
// host window we provide via FPD_ShowEditor. It lives on the UI thread; the
// WPF dispatcher pumps its messages like any other HWND.
constexpr wchar_t kFruityEditorClass[] = L"Phonic.FruityEditor";
constexpr DWORD kFruityEditorStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

HINSTANCE FruityModuleInstance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&FruityModuleInstance), &module);
    return module;
}

LRESULT CALLBACK FruityEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<FruityNode*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_CLOSE && self) { self->CloseEditor(); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    LRESULT preset = 0;
    if (PresetUiHandle(hwnd, msg, wp, lp, preset)) return preset;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool FruityNode::OpenEditor(void* owner, const wchar_t* title) {
    if (!plug) { lastError = L"The plug-in is not loaded."; return false; }
    if (editorHwnd && IsWindow(editorHwnd)) { ShowWindow(editorHwnd, SW_SHOWNORMAL); SetForegroundWindow(editorHwnd); return true; }
    static bool registered = [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = FruityEditorProc;
        wc.hInstance = FruityModuleInstance();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kFruityEditorClass;
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered) { lastError = L"Could not register the editor window class."; return false; }
    HWND wnd = CreateWindowExW(0, kFruityEditorClass, title ? title : L"Plug-in", kFruityEditorStyle,
                               CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                               static_cast<HWND>(owner), nullptr, FruityModuleInstance(), nullptr);
    if (!wnd) { lastError = L"Could not create the editor window."; return false; }
    editorHwnd = wnd;
    SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    PresetUiAttach(wnd, this);   // menu bar claims its strip before the VCL form sizes
    fhost.EditorHwnd = wnd;   // FHD_GetMainFormHandle
    if (GetEnvironmentVariableW(L"MP_FRUITY_NOCOMPAT", nullptr, 0) == 0)
        InstallFruityPins(module->dll);   // QFC pins + late IAT shims, on the UI thread
    FruityDbg("OpenEditor: iat=%d pinregion=%p destroyLies=%d postLies=%d freeRefusals=%d tid=%lu\n",
              gIatHookCount, gFruityPinRegion, gDestroyLies, gPostLies, gFreeRefusals, GetCurrentThreadId());
    bool ok = true;
    GuardedPlugCall(plug, PV_Dispatcher, FPD_ShowEditor, 0, (IntP)wnd, ok);
    if (!ok || !IsWindow(wnd)) {
        if (IsWindow(wnd)) DestroyWindow(wnd);
        editorHwnd = nullptr; fhost.EditorHwnd = nullptr;
        lastError = L"The plug-in refused to open its editor.";
        return false;
    }
    // The VCL form knows its own size; fit our window around it.
    if (HWND child = GetWindow(wnd, GW_CHILD)) {
        RECT rc; GetWindowRect(child, &rc);
        RECT wr{0, 0, rc.right - rc.left, rc.bottom - rc.top};
        AdjustWindowRectEx(&wr, kFruityEditorStyle, GetMenu(wnd) != nullptr, 0);
        SetWindowPos(wnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    PresetUiDeepDrop(wnd);   // .fst drops reach us even over VCL children
    ShowWindow(wnd, SW_SHOWNORMAL);
    return true;
}

void FruityNode::CloseEditor() {
    HWND wnd = editorHwnd;
    editorHwnd = nullptr;
    fhost.EditorHwnd = nullptr;
    if (!wnd || !IsWindow(wnd)) return;
    if (plug) {
        bool ok = true;
        GuardedPlugCall(plug, PV_Dispatcher, FPD_ShowEditor, 0, 0, ok);
    }
    SetWindowLongPtrW(wnd, GWLP_USERDATA, 0);
    DestroyWindow(wnd);
}

void FruityNode::Idle() {
    if (!plug || failed) return;
    if (editorHwnd && !IsWindow(editorHwnd)) { editorHwnd = nullptr; fhost.EditorHwnd = nullptr; }
    static int dbgTicks;
    if (++dbgTicks % 60 == 0)
        FruityDbg("Idle: destroyLies=%d postLies=%d freeRefusals=%d pinregion=%p iat=%d\n",
                  gDestroyLies, gPostLies, gFreeRefusals, gFruityPinRegion, gIatHookCount);
    bool ok = true;
    GuardedPlugCall(plug, PV_Idle, 0, 0, 0, ok);
    if (!ok) failed = true;
}

} // namespace

bool AnyFruityModuleLoaded() { return gAnyFruityLoaded; }

std::shared_ptr<Node> CreateFruityNode(int id, const std::wstring& modulePath, HostServices* host, std::wstring& error) {
    auto node = std::make_shared<FruityNode>(id);
    node->host = host;
    if (!node->Load(modulePath, error)) return nullptr;
    return node;
}

} // namespace mp
