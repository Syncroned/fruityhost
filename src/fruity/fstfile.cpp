#include "fstfile.h"
#include <cstdio>
#include <cstring>

namespace mp {
namespace {
uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
// FL's event stream encodes variable-length payloads with a little-endian
// 7-bit varint (continuation bit = more bytes follow).
bool ReadVarint(const uint8_t* d, size_t n, size_t& i, uint32_t& out) {
    uint32_t v = 0; int shift = 0;
    while (i < n && shift < 32) {
        uint8_t b = d[i++];
        v |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { out = v; return true; }
        shift += 7;
    }
    return false;
}
bool WideString(const uint8_t* p, size_t n, std::wstring& out) {
    if (n < 2 || (n & 1)) return false;
    out.assign(reinterpret_cast<const wchar_t*>(p), n / 2);
    while (!out.empty() && out.back() == 0) out.pop_back();
    return true;
}
} // namespace

bool ParseFstFile(const std::wstring& path, FstChunk& out) {
    out = {};
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 256u * 1024 * 1024) { fclose(f); return false; }
    std::vector<uint8_t> d((size_t)len);
    bool rd = fread(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    if (!rd || d.size() < 20 || memcmp(d.data(), "FLhd", 4) != 0) return false;
    size_t i = 8 + U32(d.data() + 4);                      // skip FLhd
    if (i + 8 > d.size() || memcmp(d.data() + i, "FLdt", 4) != 0) return false;
    i += 8;
    const size_t end = i + U32(d.data() + i - 4);
    if (end > d.size()) return false;
    while (i < end) {
        uint8_t ev = d[i++];
        size_t payload = 0;
        if (ev < 64) payload = 1;
        else if (ev < 128) payload = 2;
        else if (ev < 192) payload = 4;
        else {
            uint32_t vlen;
            if (!ReadVarint(d.data(), end, i, vlen)) return false;
            payload = vlen;
        }
        if (i + payload > end) return false;
        switch (ev) {
        case 201: WideString(d.data() + i, payload, out.pluginName); break;
        case 203: WideString(d.data() + i, payload, out.presetName); break;
        case 213: out.data.assign(d.data() + i, d.data() + i + payload); break;
        default: break;
        }
        i += payload;
    }
    return true;
}
} // namespace mp
