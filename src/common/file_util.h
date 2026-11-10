// Small file helpers that take wide (UTF-16) Windows paths portably.
#pragma once
#include <cstdio>
#include <string>

namespace Aetheron {

inline bool ReadTextFile(const std::wstring& path, std::string* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out->clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

}  // namespace Aetheron
