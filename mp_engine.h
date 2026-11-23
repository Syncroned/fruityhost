#pragma once
// C ABI of the Phonic native audio engine. All functions are called from
// the application's UI thread unless noted otherwise; the real-time audio
// thread is private to the engine.
#include <stdint.h>

#ifdef MP_BUILD
#define MP_API extern "C" __declspec(dllexport)
#else
#define MP_API extern "C" __declspec(dllimport)
#endif

enum { MP_MAX_PORTS = 4, MP_NAME_LEN = 128 };

enum MP_Result {
    MP_OK = 0,
    MP_E_INVALID = -1,      // bad argument / unknown node / bad port
    MP_E_CYCLE = -2,        // connection would create a feedback loop
    MP_E_EXISTS = -3,       // id or connection already exists
    MP_E_LOAD = -4,         // plug-in module/class could not be loaded
    MP_E_DEVICE = -5,       // audio device could not be opened
    MP_E_STATE = -6,        // operation not allowed in the current state
    MP_E_UNSUPPORTED = -7,
    MP_E_BUFFER = -8,       // caller buffer too small (required size returned separately)
};

enum MP_ParamFlags {
    MP_PARAM_AUTOMATABLE = 1, MP_PARAM_READONLY = 2, MP_PARAM_BYPASS = 4,
    MP_PARAM_HIDDEN = 8, MP_PARAM_NORMALIZED = 16, MP_PARAM_LIST = 32,
};

typedef struct MP_ParamInfo {
    uint32_t id;
    int32_t flags;
    int32_t stepCount;       // 0 = continuous
    double minValue, maxValue, defaultValue;
    wchar_t name[MP_NAME_LEN];
    wchar_t units[32];
} MP_ParamInfo;

typedef struct MP_NativeNodeType {
    char type[32];                 // node type id passed to MP_AddNode
    wchar_t title[64];             // node library display name
    wchar_t description[256];
} MP_NativeNodeType;

typedef struct MP_NodeInfo {
    int32_t inputs, outputs;
    int32_t inputChannels[MP_MAX_PORTS];
    int32_t outputChannels[MP_MAX_PORTS];
    int32_t latencySamples;
    int32_t hasEditor;
    int32_t paramCount;
    int32_t isPlugin;
    wchar_t inputNames[MP_MAX_PORTS][32];
    wchar_t outputNames[MP_MAX_PORTS][32];
} MP_NodeInfo;

enum MP_NodeState { MP_NODE_IDLE = 0, MP_NODE_ACTIVE = 1, MP_NODE_BYPASSED = 2, MP_NODE_FAILED = 3, MP_NODE_MISSING = 4 };

typedef struct MP_NodeStatus {
    int32_t state;           // MP_NodeState
    int32_t inPlan;          // node participates in the current processing order
    float inPeak[2], outPeak[2]; // peak since the previous query (linear)
    float gainReductionDb;   // dynamics nodes
    int32_t paramsChanged;   // controller/editor changed parameters since previous query
    int32_t editorOpen;
} MP_NodeStatus;

typedef struct MP_EngineStatus {
    int32_t running;
    int32_t deviceLost;      // audio device disappeared / was invalidated
    int32_t sampleRate;
    int32_t blockFrames;     // render period
    int32_t inputChannels, outputChannels;
    int32_t inputLatencyMs, outputLatencyMs;
    int64_t underruns, overruns, callbacks;
    float cpuLoad;           // 0..1 of the period spent processing (smoothed)
    float inPeak[2], outPeak[2];
    int32_t clipped;         // output limiter/clipper engaged since previous query
    int32_t planNodes;
    int32_t lastHr;
    int32_t monitorRunning;  // monitor stream is rendering on its device
    int32_t monitorLost;     // monitor device disappeared / was invalidated
    int32_t injectionConnected; // endpoints currently consuming the injected mix
} MP_EngineStatus;

typedef struct MP_Engine MP_Engine;

MP_API MP_Engine* MP_Create(double sampleRate, int32_t maxBlock);
MP_API void MP_Destroy(MP_Engine* engine);
MP_API int32_t MP_GetLastError(MP_Engine* engine, wchar_t* buffer, int32_t chars);

// Node types: "input", "output", "gain", "mixer", "mix", "router", "splitlr",
// "meter", "gate", "eq", "compressor", "limiter", plus any node plug-ins found
// in plugins\ next to the engine or %LOCALAPPDATA%\Phonic\plugins.
MP_API int32_t MP_AddNode(MP_Engine* engine, int32_t nodeId, const char* type, int32_t channels);
// All native node types (built-in + node plug-ins) for the node library.
// Returns the type count; copies min(count, capacity) entries.
MP_API int32_t MP_GetNativeNodeTypes(MP_NativeNodeType* out, int32_t capacity);
MP_API int32_t MP_AddPluginNode(MP_Engine* engine, int32_t nodeId, const wchar_t* modulePath, const char* classId);
// Loads an Image-Line Fruity effect DLL (*_x64.dll). Experimental.
MP_API int32_t MP_AddFruityNode(MP_Engine* engine, int32_t nodeId, const wchar_t* modulePath);
// Stand-in for a plug-in that is not installed: keeps ports so connections survive; passes audio through.
MP_API int32_t MP_AddPlaceholderNode(MP_Engine* engine, int32_t nodeId, int32_t inputs, const int32_t* inChannels, int32_t outputs, const int32_t* outChannels);
MP_API int32_t MP_RemoveNode(MP_Engine* engine, int32_t nodeId);
MP_API int32_t MP_GetNodeInfo(MP_Engine* engine, int32_t nodeId, MP_NodeInfo* info);
MP_API int32_t MP_GetNodeStatus(MP_Engine* engine, int32_t nodeId, MP_NodeStatus* status);
MP_API int32_t MP_SetBypass(MP_Engine* engine, int32_t nodeId, int32_t bypass);

MP_API int32_t MP_Connect(MP_Engine* engine, int32_t srcNode, int32_t srcPort, int32_t dstNode, int32_t dstPort);
MP_API int32_t MP_Disconnect(MP_Engine* engine, int32_t srcNode, int32_t srcPort, int32_t dstNode, int32_t dstPort);
MP_API int32_t MP_WouldCycle(MP_Engine* engine, int32_t srcNode, int32_t dstNode);
// Validates the graph and publishes a new processing order to the audio thread (lock-free swap).
MP_API int32_t MP_Commit(MP_Engine* engine);

MP_API int32_t MP_GetParamCount(MP_Engine* engine, int32_t nodeId);
MP_API int32_t MP_GetParamInfo(MP_Engine* engine, int32_t nodeId, int32_t index, MP_ParamInfo* info);
MP_API int32_t MP_GetParam(MP_Engine* engine, int32_t nodeId, uint32_t paramId, double* value);
MP_API int32_t MP_SetParam(MP_Engine* engine, int32_t nodeId, uint32_t paramId, double value);
MP_API int32_t MP_FormatParam(MP_Engine* engine, int32_t nodeId, uint32_t paramId, double value, wchar_t* buffer, int32_t chars);

// Opaque state blob (plug-in component + controller state, or native parameters).
// Returns the required size; MP_E_BUFFER if capacity is too small.
MP_API int32_t MP_GetState(MP_Engine* engine, int32_t nodeId, uint8_t* buffer, int32_t capacity, int32_t* size);
MP_API int32_t MP_SetState(MP_Engine* engine, int32_t nodeId, const uint8_t* data, int32_t size);

MP_API int32_t MP_OpenEditor(MP_Engine* engine, int32_t nodeId, void* ownerWindow, const wchar_t* title);
MP_API int32_t MP_CloseEditor(MP_Engine* engine, int32_t nodeId);

// Loads an FL Studio preset file (.fst) into a plug-in node. The .fst plug-in
// state chunk is fed to the plug-in's state restore, exactly as FL does when a
// preset is picked or dropped on the plug-in.
MP_API int32_t MP_LoadPreset(MP_Engine* engine, int32_t nodeId, const wchar_t* path);

// Devices. Empty / null id selects the Windows default device.
MP_API int32_t MP_Start(MP_Engine* engine, const wchar_t* inputId, const wchar_t* outputId, int32_t periodMs);
MP_API void MP_Stop(MP_Engine* engine);
MP_API int32_t MP_GetEngineStatus(MP_Engine* engine, MP_EngineStatus* status);
MP_API void MP_SetMaster(MP_Engine* engine, float gainDb, int32_t mute, int32_t limiter);
// Host tempo (BPM, clamped to 10..999) reported to tempo-synced plug-ins. The
// transport is always playing; its position starts at 0 when the engine is created.
MP_API void MP_SetTempo(MP_Engine* engine, double bpm);

// Call as the very last step of process shutdown (after MP_Destroy). If any
// Fruity plug-in DLL was loaded, the process is terminated with `exitCode`
// without running DLL shutdown code: Image-Line's Delphi runtime crashes in its
// finalization at normal process exit ("Runtime error 216"). Otherwise returns.
MP_API void MP_ExitProcess(int32_t exitCode);

// Optional monitor output: the finished stereo mix also plays on a second
// render device (e.g. headphones) while the main output goes elsewhere
// (e.g. a virtual cable). Empty / null id selects the default render device.
MP_API int32_t MP_SetMonitor(MP_Engine* engine, const wchar_t* deviceId);
MP_API void MP_StopMonitor(MP_Engine* engine);

// Microphone injection: publishes the finished stereo mix into the shared
// transport consumed by the capture-endpoint APO (Phonic.Apo.dll) on the
// given recording endpoint. replace != 0 mutes the raw microphone in the APO so
// applications receive only the processed mix. Requires the endpoint to be set
// up by the installer first.
MP_API int32_t MP_SetInjection(MP_Engine* engine, const wchar_t* captureEndpointId, int32_t replace);
MP_API void MP_StopInjection(MP_Engine* engine);

// Housekeeping on the UI thread: frees retired graphs, forwards plug-in output
// parameters to controllers and handles plug-in restart requests.
MP_API void MP_Idle(MP_Engine* engine);

// Offline processing for tests and rendering. Only allowed while no device is running.
// input: interleaved, inChannels (1 or 2); output: interleaved stereo.
MP_API int32_t MP_ProcessOffline(MP_Engine* engine, const float* input, int32_t inChannels, float* output, int32_t frames);

// Diagnostics used by the lifecycle tests.
MP_API int32_t MP_DebugLoadedModules(void);
MP_API int32_t MP_DebugRetiredPlans(MP_Engine* engine);
