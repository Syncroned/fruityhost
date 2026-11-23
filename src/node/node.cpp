#include "node.h"
#include <cstdio>
#include <cwchar>
#include <sstream>

namespace mp {

void Node::AddParam(ParamDesc desc) {
    desc.id = static_cast<uint32_t>(params.size());
    if (!desc.labels.empty()) { desc.flags |= MP_PARAM_LIST; desc.steps = static_cast<int>(desc.labels.size()) - 1; }
    values.push_back(std::make_unique<AtomicFloat>(static_cast<float>(desc.defaultValue)));
    params.push_back(std::move(desc));
}

bool Node::GetParamInfo(int index, MP_ParamInfo& info) const {
    if (index < 0 || index >= static_cast<int>(params.size())) return false;
    const auto& p = params[index];
    info = {};
    info.id = p.id; info.flags = p.flags; info.stepCount = p.steps;
    info.minValue = p.minValue; info.maxValue = p.maxValue; info.defaultValue = p.defaultValue;
    wcsncpy_s(info.name, p.name.c_str(), _TRUNCATE);
    wcsncpy_s(info.units, p.units.c_str(), _TRUNCATE);
    return true;
}

bool Node::GetParam(uint32_t pid, double& value) const {
    if (pid >= values.size()) return false;
    value = values[pid]->Load();
    return true;
}

bool Node::SetParam(uint32_t pid, double value) {
    if (pid >= values.size() || !std::isfinite(value)) return false;
    const auto& p = params[pid];
    value = std::clamp(value, p.minValue, p.maxValue);
    if (p.steps > 0) value = std::round(value);
    values[pid]->Store(static_cast<float>(value));
    return true;
}

std::wstring Node::FormatParam(uint32_t pid, double value) const {
    if (pid >= params.size()) return L"";
    const auto& p = params[pid];
    if (!p.labels.empty()) {
        int i = std::clamp(static_cast<int>(std::lround(value - p.minValue)), 0, static_cast<int>(p.labels.size()) - 1);
        return p.labels[i];
    }
    wchar_t text[64];
    double a = std::fabs(value);
    swprintf_s(text, a >= 100 ? L"%.0f" : a >= 10 ? L"%.1f" : L"%.2f", value);
    return p.units.empty() ? std::wstring(text) : std::wstring(text) + L" " + p.units;
}

std::vector<uint8_t> Node::GetState() const {
    std::ostringstream s;
    s.precision(9);
    s << "MPN1\n";
    for (size_t i = 0; i < values.size(); ++i) s << i << '=' << values[i]->Load() << '\n';
    auto text = s.str();
    return {text.begin(), text.end()};
}

bool Node::SetState(const uint8_t* data, size_t size) {
    std::istringstream s(std::string(reinterpret_cast<const char*>(data), size));
    std::string line;
    if (!std::getline(s, line) || line != "MPN1") return false;
    while (std::getline(s, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        SetParam(static_cast<uint32_t>(std::stoul(line.substr(0, eq))), std::stod(line.substr(eq + 1)));
    }
    paramsChanged++;
    return true;
}

} // namespace mp
