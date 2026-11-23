#pragma once
#include "mp_engine.h"
#include "rt.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace mp {

struct PortDesc { int channels; std::wstring name; };

struct ParamDesc {
    uint32_t id;
    std::wstring name, units;
    double minValue, maxValue, defaultValue;
    int steps = 0;
    int flags = MP_PARAM_AUTOMATABLE;
    std::vector<std::wstring> labels; // list parameters
};

// Free-running host transport: always "playing" at the user's tempo so
// tempo-synced plug-ins (Gross Beat, LFO effects, delays) have a timeline.
// Values describe the first sample of the current block.
struct Transport {
    double tempo = 120;      // BPM
    double beats = 0;        // quarter notes since the engine started
    int64_t samples = 0;     // samples since the engine started
};

// Everything a node sees during one processing call. Channel pointer arrays
// are prepared by the compiled plan; no allocation happens per block.
struct ProcessContext {
    int frames = 0;
    Transport transport;
    const float* const* in[MP_MAX_PORTS] = {};
    float* const* out[MP_MAX_PORTS] = {};
    int inChannels[MP_MAX_PORTS] = {};
    int outChannels[MP_MAX_PORTS] = {};
    // Device endpoints (used by the input/output nodes only).
    const float* deviceIn[2] = {};
    int deviceInChannels = 0;
    float* deviceOut[2] = {};
};

// Services the engine offers to nodes (UI thread only).
struct HostServices {
    virtual ~HostServices() = default;
    // Returns once the audio thread no longer runs any node marked suspended.
    virtual void AudioBarrier() = 0;
    virtual void RequestRecompile() = 0;
    virtual double SampleRate() const = 0;
    virtual int MaxBlock() const = 0;
};

class Node {
public:
    explicit Node(int id) : id(id) {}
    virtual ~Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    const int id;
    std::vector<PortDesc> inputs, outputs;

    virtual const char* Type() const = 0;
    virtual bool IsPlugin() const { return false; }
    virtual bool IsMissing() const { return false; }
    // Sinks are kept in the processing order even when they do not feed the output.
    virtual bool IsSink() const { return false; }

    // Control thread while the node is not being processed.
    virtual void Prepare(double sampleRate, int maxBlock) { (void)sampleRate; (void)maxBlock; }
    virtual void Release() {}
    // Audio thread.
    virtual void Process(ProcessContext& ctx) = 0;

    virtual int ParamCount() const { return static_cast<int>(params.size()); }
    virtual bool GetParamInfo(int index, MP_ParamInfo& info) const;
    virtual bool GetParam(uint32_t pid, double& value) const;
    virtual bool SetParam(uint32_t pid, double value);
    virtual std::wstring FormatParam(uint32_t pid, double value) const;
    virtual std::vector<uint8_t> GetState() const;
    virtual bool SetState(const uint8_t* data, size_t size);
    // FL Studio preset files (.fst): listed on the editor's Presets menu and
    // accepted via drag & drop. Plug-in nodes override both.
    virtual std::vector<std::wstring> PresetFiles() const { return {}; }
    virtual bool LoadPresetFile(const std::wstring& path) { (void)path; return false; }

    virtual int LatencySamples() const { return 0; }
    virtual bool HasEditor() const { return false; }
    virtual bool OpenEditor(void* owner, const wchar_t* title) { (void)owner; (void)title; return false; }
    virtual void CloseEditor() {}
    virtual bool EditorOpen() const { return false; }
    virtual void Idle() {}

    HostServices* host = nullptr;
    std::atomic<bool> bypass{false}, suspended{false}, failed{false};
    AtomicFloat inPeak[2], outPeak[2], gainReduction;
    std::atomic<int> paramsChanged{0};
    std::wstring lastError; // UI thread only

protected:
    // Native parameters: descriptions are fixed at construction, values are
    // lock-free floats written by the UI thread and read by the audio thread.
    void AddParam(ParamDesc desc);
    float P(uint32_t index) const { return values[index]->Load(); }
    std::vector<ParamDesc> params;
    std::vector<std::unique_ptr<AtomicFloat>> values;
};

std::shared_ptr<Node> CreateNativeNode(int id, const std::string& type, int channels);

} // namespace mp
