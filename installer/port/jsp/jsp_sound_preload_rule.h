// jsp_sound_preload_rule.h -- FIX (online fight hitches, test8 bundles): the pure rules of the sound archive preload (jsp_sound_preload.cpp).
// No OS calls: installer/tools/sound_preload_test.cpp tests them on the host.
//
// Which file: the game's sound archive, partition_op_sound_ps3.cpk (381 MB in the US game; the CRI FS file threads stream voices and music from it
// during a fight). How much: JSP_SOUND_PRELOAD_MB (default 512, 0 = off), never more than the file, and never more than half of the RAM that is free
// above a 1 GB reserve when the preload starts (a PC short on RAM keeps its headroom; the preload is then partial or skipped).
#pragma once
#include <cstdint>
#include <cstring>

namespace jspre {

inline constexpr long long kDefaultMb = 512;          // JSP_SOUND_PRELOAD_MB when unset or not a number
inline constexpr long long kMaxMb = 4096;             // a larger value is taken as this
inline constexpr uint64_t kReserveBytes = 1ull << 30; // free RAM the preload never takes
inline constexpr uint64_t kAlign = 64ull * 1024;      // preload lengths are whole 64 KB blocks (the Windows view granularity)
inline constexpr const char* kArchive = "partition_op_sound_ps3.cpk";

// The guest path names the sound archive (any mount: /dev_bdvd or the hdd0 install copy). Case-insensitive, whole file name.
inline bool is_sound_archive(const char* path)
{
    if (!path) return false;
    const size_t n = std::strlen(path), k = std::strlen(kArchive);
    if (n < k) return false;
    const char* tail = path + (n - k);
    if (n > k && tail[-1] != '/' && tail[-1] != '\\') return false;
    for (size_t i = 0; i < k; ++i) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != kArchive[i]) return false;
    }
    return true;
}

// JSP_SOUND_PRELOAD_MB: digits only (surrounding spaces allowed); unset, empty or anything else gives the default; 0 = off.
inline long long parse_budget_mb(const char* s)
{
    if (!s) return kDefaultMb;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) return kDefaultMb;
    long long v = 0;
    const char* p = s;
    for (; *p >= '0' && *p <= '9'; ++p) { v = v * 10 + (*p - '0'); if (v > kMaxMb) v = kMaxMb; }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (p == s || *p) return kDefaultMb;
    return v;
}

// Bytes to preload from the start of the file. avail_phys = free physical RAM in bytes (0 = unknown: no RAM cap).
inline uint64_t preload_bytes(uint64_t file_size, long long budget_mb, uint64_t avail_phys)
{
    if (budget_mb <= 0 || file_size == 0) return 0;
    uint64_t n = (uint64_t)budget_mb << 20;
    if (n > file_size) n = file_size;
    if (avail_phys) {
        const uint64_t room = avail_phys > kReserveBytes ? (avail_phys - kReserveBytes) / 2 : 0;
        if (n > room) n = room;
    }
    if (n < file_size) n -= n % kAlign;   // a partial preload ends on a block; the whole file is taken as it is
    return n;
}

}  // namespace jspre
