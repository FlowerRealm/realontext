#pragma once
#include <array>
#include <string>

// A git object id. Content address of every blob and commit the index refers to.
using Oid = std::array<unsigned char, 20>;

inline std::string hex(const Oid& id)
{
    static const char digits[] = "0123456789abcdef";
    std::string s(40, '0');
    for (size_t i = 0; i < id.size(); i++) {
        s[2 * i] = digits[id[i] >> 4];
        s[2 * i + 1] = digits[id[i] & 15];
    }
    return s;
}

// A file version: one path holding one blob. The unit branch bitmaps count
// (docs/modules/store.md); ingest/ produces it, store/ persists it.
struct FileVersion {
    std::string path;
    Oid blob;
};
