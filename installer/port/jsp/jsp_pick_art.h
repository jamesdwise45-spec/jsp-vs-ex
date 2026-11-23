// ENHANCEMENT (after-match menu in the game's own look, JSP_ROLLBACK_PICK=1): reads the game's own menu art from the player's installed
// archives at run time. Header only, no Windows, toolkit or game calls: the caller gives a jmods::Io that reads the CPK files (the mods
// loader's interface; DirIo below reads them from a folder with stdio) and gets decoded RGBA images and the system font's glyph table,
// kept in memory only. Nothing is written anywhere, and the repository ships no game data: only the names below and the format rules. Host-tested by installer/tools/pick_art_test.cpp.
//
// Formats (desk notes: D:\jsp-audit\pick-ui\ART-REVIEW.md):
//   CPK      jmods::load_archive (TOC rows "Dir/File", entry bytes at toc_offset + FileOffset)
//   $CMP     the game's compression: 0x20-byte header (+0x10 BE unpacked size), then chunks {tag, u32 unpacked, u32 size incl. its 16-byte
//            header, u32}: $CL0 LZ tokens (even t: t>>1 literal bytes; odd t: copy (t>>1)&31 bytes from distance ((t>>6)<<8)|next byte,
//            within the chunk), $CLH = a $CH0 Huffman block that decodes to a $CL0 chunk, $CR0 raw, $CH0 Huffman bytes ({u32 out, u32 in,
//            u32 count} then count x {u16 LE code, u8 len<<3, u8 symbol}, bits MSB first; count 1 = one repeated symbol)
//   STPK     package: u32 count at +8, entries at 0x10 + 0x30 i {u32 BE offset, u32 BE size, 8 x 0, char name[32]}; an entry may be $CMP
//   SRD      blocks {char tag[4], u32 BE data, u32 BE sub-data, u32}, data and sub-data padded to 16; $TXR {u32, u16 swizzle, u16 w, u16 h,
//            u16 scanline, u8 format (0x0F DXT1, 0x11 DXT5, 0x01 ARGB8 Morton order)} with a $RSI sub-block {+16 u32 offset (low 28 bits)
//            into the .srdv, +20 u32 length, +0x20 name}
//   SpFt     font (in the .stx SRD): +0x14 glyph count, +0x18 glyph table, +0x1C char bit map (u32 BE words, bit i of word k = char 32k+i),
//            +0x24 u8 line height, +0x26 u8 baseline; glyph i (the i-th set bit) = 8 bytes {u8 w, u24 (atlas y << 12 | atlas x), u8 h,
//            s8 left bearing, s8 right bearing, u8 top offset in the line}; the atlas is the font texture (DXT1, white on black)
#ifndef JSP_PICK_ART_H
#define JSP_PICK_ART_H

#include "jsp_mods_core.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace jpart {

using Bytes = std::vector<uint8_t>;
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

// ---- $CMP ---------------------------------------------------------------------------------------------------------------------------
inline bool cmp_lz(const uint8_t* d, size_t p, size_t end, Bytes& out, size_t base)
{
    while (p < end) {
        const uint8_t t = d[p++];
        if (!(t & 1)) {
            const size_t n = t >> 1;
            if (p + n > end) return false;
            out.insert(out.end(), d + p, d + p + n); p += n;
        } else {
            if (p >= end) return false;
            const size_t n = (t >> 1) & 0x1F, dist = size_t(t >> 6) << 8 | d[p++];
            if (dist == 0 || dist > out.size() - base) return false;
            for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
        }
    }
    return true;
}
// A $CH0 block at d[p..lim): its decoded bytes into `res`.
inline bool cmp_huff(const uint8_t* d, size_t p, size_t lim, Bytes& res)
{
    if (p + 16 > lim || memcmp(d + p, "$CH0", 4) != 0) return false;
    const uint32_t out_n = rd32(d + p + 4), in_n = rd32(d + p + 8), cnt = rd32(d + p + 12);
    if (out_n > (64u << 20) || in_n > lim - p || cnt == 0 || cnt > 256) return false;
    res.clear(); res.reserve(out_n);
    if (cnt == 1) { if (p + 20 > lim) return false; res.assign(out_n, d[p + 19]); return true; }
    // a binary trie: node k has children kid[2k], kid[2k+1]; > 0 node index, < 0 a leaf -(symbol + 1), 0 none
    std::vector<int32_t> kid(2, 0);
    size_t q = p + 16;
    if (q + size_t(cnt) * 4 > p + in_n) return false;
    for (uint32_t k = 0; k < cnt; k++, q += 4) {
        const uint32_t code = uint32_t(d[q]) | uint32_t(d[q + 1]) << 8, len = d[q + 2] >> 3, sym = d[q + 3];
        if (len == 0 || len > 24) return false;
        int32_t node = 0;
        for (int b = int(len) - 1; b >= 0; b--) {
            const size_t slot = size_t(node) * 2 + ((code >> b) & 1);
            if (b == 0) { if (kid[slot] != 0) return false; kid[slot] = -int32_t(sym) - 1; break; }
            if (kid[slot] < 0) return false;
            if (kid[slot] == 0) { kid[slot] = int32_t(kid.size() / 2); kid.push_back(0); kid.push_back(0); }
            node = kid[slot];
        }
    }
    size_t bit = q * 8;
    const size_t end = (p + in_n) * 8;
    int32_t node = 0;
    while (res.size() < out_n) {
        if (bit >= end) return false;
        const int32_t nx = kid[size_t(node) * 2 + ((d[bit >> 3] >> (7 - (bit & 7))) & 1)]; bit++;
        if (nx == 0) return false;
        if (nx < 0) { res.push_back(uint8_t(-nx - 1)); node = 0; } else node = nx;
    }
    return true;
}
inline bool cmp_decode(const uint8_t* d, size_t n, Bytes& out)
{
    if (n < 0x20 || memcmp(d, "$CMP", 4) != 0) return false;
    const uint32_t total = rd32(d + 16);
    if (total > (256u << 20)) return false;
    out.clear(); out.reserve(total);
    size_t p = 0x20;
    Bytes h;
    while (p + 16 <= n && d[p] == '$') {
        const uint32_t a = rd32(d + p + 4), sz = rd32(d + p + 8);
        if (sz < 16 || p + sz > n) return false;
        const size_t base = out.size();
        if (!memcmp(d + p, "$CL0", 4)) { if (!cmp_lz(d, p + 16, p + sz, out, base)) return false; }
        else if (!memcmp(d + p, "$CLH", 4)) {
            if (!cmp_huff(d, p + 16, p + sz, h) || h.size() < 16 || memcmp(h.data(), "$CL0", 4) != 0) return false;
            if (!cmp_lz(h.data(), 16, h.size(), out, base)) return false;
        } else if (!memcmp(d + p, "$CR0", 4)) out.insert(out.end(), d + p + 16, d + p + sz);
        else if (!memcmp(d + p, "$CH0", 4)) { if (!cmp_huff(d, p, p + sz, h)) return false; out.insert(out.end(), h.begin(), h.end()); }
        else return false;
        if (out.size() - base != a) return false;
        p += sz;
    }
    return out.size() == total;
}
// $CMP -> plain; anything else unchanged.
inline bool plain(Bytes& b)
{
    if (b.size() < 4 || memcmp(b.data(), "$CMP", 4) != 0) return true;
    Bytes o;
    if (!cmp_decode(b.data(), b.size(), o)) return false;
    b.swap(o);
    return true;
}

// ---- STPK ---------------------------------------------------------------------------------------------------------------------------
// The entry `name` of a package (names are cut at 32 characters there), made plain.
inline bool stpk_get(const Bytes& pak, const std::string& name, Bytes& out)
{
    if (pak.size() < 0x10 || memcmp(pak.data(), "STPK", 4) != 0) return false;
    const uint32_t cnt = rd32(&pak[8]);
    if (cnt > 100000 || 0x10 + size_t(cnt) * 0x30 > pak.size()) return false;
    const std::string want = name.substr(0, 32);
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t* e = &pak[0x10 + size_t(i) * 0x30];
        const char* nm = (const char*)e + 0x10;
        const size_t len = strnlen(nm, 32);
        if (std::string(nm, len) != want) continue;
        const uint32_t off = rd32(e), sz = rd32(e + 4);
        if (uint64_t(off) + sz > pak.size()) return false;
        out.assign(pak.begin() + off, pak.begin() + off + sz);
        return plain(out);
    }
    return false;
}

// ---- SRD textures ---------------------------------------------------------------------------------------------------------------------
struct TexInfo { std::string name; int w = 0, h = 0, fmt = 0; uint32_t off = 0, len = 0; };
inline std::vector<TexInfo> srd_textures(const Bytes& s)
{
    std::vector<TexInfo> out;
    auto pad = [](size_t n) { return (n + 15) & ~size_t(15); };
    size_t p = 0;
    while (p + 16 <= s.size()) {
        const size_t dl = rd32(&s[p + 4]), sl = rd32(&s[p + 8]);
        const size_t dp = p + 16, sp = dp + pad(dl);
        if (sp > s.size() || sp + sl > s.size()) break;
        if (!memcmp(&s[p], "$TXR", 4) && dl >= 13) {
            TexInfo t;
            t.w = int(rd16(&s[dp + 6])); t.h = int(rd16(&s[dp + 8])); t.fmt = s[dp + 12];
            for (size_t q = sp; q + 16 <= sp + sl;) {   // the $RSI sub-block
                const size_t qd = rd32(&s[q + 4]);
                if (!memcmp(&s[q], "$RSI", 4) && qd >= 0x21 && q + 16 + qd <= s.size()) {
                    const uint8_t* r = &s[q + 16];
                    t.off = rd32(r + 16) & 0x0FFFFFFFu; t.len = rd32(r + 20);
                    t.name.assign((const char*)r + 0x20, strnlen((const char*)r + 0x20, qd - 0x20));
                    break;
                }
                q += 16 + pad(qd) + pad(rd32(&s[q + 8]));
            }
            out.push_back(t);
        }
        p = sp + pad(sl);
    }
    return out;
}

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // straight RGBA8, rows top to bottom
    bool ok() const { return w > 0 && h > 0 && px.size() == size_t(w) * size_t(h) * 4; }
};
inline void rgb565(uint32_t v, uint8_t* c) { const uint32_t r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31; c[0] = uint8_t(r << 3 | r >> 2); c[1] = uint8_t(g << 2 | g >> 4); c[2] = uint8_t(b << 3 | b >> 2); }
inline Image decode_dxt(const uint8_t* d, size_t n, int w, int h, bool dxt5)
{
    Image im;
    const int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = dxt5 ? 16 : 8;
    if (w <= 0 || h <= 0 || n < size_t(bw) * size_t(bh) * size_t(bs)) return im;
    im.w = w; im.h = h; im.px.assign(size_t(w) * size_t(h) * 4, 0);
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t* b = d + (size_t(by) * bw + bx) * bs;
            uint8_t al[16];
            if (dxt5) {
                const uint32_t a0 = b[0], a1 = b[1];
                uint8_t pal[8] = { uint8_t(a0), uint8_t(a1) };
                if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = uint8_t(((6 - i) * a0 + i * a1) / 6);
                else { for (int i = 1; i < 5; i++) pal[i + 1] = uint8_t(((4 - i) * a0 + i * a1) / 4); pal[6] = 0; pal[7] = 255; }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) al[i] = pal[(bits >> (3 * i)) & 7];
                b += 8;
            } else memset(al, 255, 16);
            const uint32_t c0 = uint32_t(b[0]) | uint32_t(b[1]) << 8, c1 = uint32_t(b[2]) | uint32_t(b[3]) << 8;
            const uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
            uint8_t cs[4][4] = {};
            rgb565(c0, cs[0]); rgb565(c1, cs[1]);
            const bool four = c0 > c1 || dxt5;
            for (int k = 0; k < 3; k++) {
                cs[2][k] = uint8_t(four ? (2 * cs[0][k] + cs[1][k]) / 3 : (cs[0][k] + cs[1][k]) / 2);
                cs[3][k] = uint8_t(four ? (cs[0][k] + 2 * cs[1][k]) / 3 : 0);
            }
            for (int i = 0; i < 16; i++) {
                const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                const uint32_t k = idx >> (2 * i) & 3;
                uint8_t* o = &im.px[(size_t(y) * w + x) * 4];
                o[0] = cs[k][0]; o[1] = cs[k][1]; o[2] = cs[k][2];
                o[3] = (!dxt5 && !four && k == 3) ? 0 : al[i];
            }
        }
    return im;
}
// ARGB8 in Morton (Z) order, as the PS3 swizzles a power-of-two texture.
inline Image decode_argb_morton(const uint8_t* d, size_t n, int w, int h)
{
    Image im;
    if (w <= 0 || h <= 0 || (w & (w - 1)) || (h & (h - 1)) || n < size_t(w) * size_t(h) * 4) return im;
    im.w = w; im.h = h; im.px.assign(size_t(w) * size_t(h) * 4, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t m = 0, bit = 0, xx = uint32_t(x), yy = uint32_t(y);
            for (int mw = w, mh = h; mw > 1 || mh > 1;) {
                if (mw > 1) { m |= (xx & 1) << bit++; xx >>= 1; mw >>= 1; }
                if (mh > 1) { m |= (yy & 1) << bit++; yy >>= 1; mh >>= 1; }
            }
            const uint8_t* s = d + size_t(m) * 4;
            uint8_t* o = &im.px[(size_t(y) * w + x) * 4];
            o[0] = s[1]; o[1] = s[2]; o[2] = s[3]; o[3] = s[0];
        }
    return im;
}
inline Image decode_tex(const TexInfo& t, const Bytes& v)
{
    if (uint64_t(t.off) + t.len > v.size()) return Image();
    const uint8_t* d = v.data() + t.off;
    if (t.fmt == 0x0F) return decode_dxt(d, t.len, t.w, t.h, false);
    if (t.fmt == 0x11) return decode_dxt(d, t.len, t.w, t.h, true);
    if (t.fmt == 0x01) return decode_argb_morton(d, t.len, t.w, t.h);
    return Image();
}
// The texture called `name` ("" = the first) of an SRD / SRDV pair.
inline Image srd_image(const Bytes& srd, const Bytes& srdv, const std::string& name)
{
    for (const TexInfo& t : srd_textures(srd)) if (name.empty() || t.name == name) return decode_tex(t, srdv);
    return Image();
}

// ---- SpFt (the system font) -----------------------------------------------------------------------------------------------------------
struct Glyph { int w = 0, ax = 0, ay = 0, h = 0, l = 0, r = 0, top = 0; bool ok = false; };
struct Font {
    Image atlas;          // coverage in alpha (white on black in the game's texture)
    Glyph g[95];          // ASCII 0x20..0x7E
    int line = 0, base = 0, cap = 0;
    bool ok() const { return atlas.ok() && line > 0 && g['A' - 32].ok; }
    float width(const std::string& s, float scale) const
    {
        float w = 0;
        for (unsigned char c : s) { const Glyph& q = g[(c >= 32 && c < 127 ? c : '?') - 32]; if (q.ok) w += float(q.l + q.w + q.r); }
        return w * scale;
    }
};
inline bool spft_parse(const Bytes& stx, const Image& tex, Font& f)
{
    f = Font();
    size_t b = 0;
    for (; b + 0x28 <= stx.size(); b += 4) if (!memcmp(&stx[b], "SpFt", 4)) break;
    if (b + 0x28 > stx.size() || !tex.ok()) return false;
    const uint32_t count = rd32(&stx[b + 0x14]), gt = rd32(&stx[b + 0x18]), bm = rd32(&stx[b + 0x1C]);
    f.line = stx[b + 0x24]; f.base = stx[b + 0x26];
    if (count == 0 || count > 65536 || b + gt + size_t(count) * 8 > stx.size() || b + bm + 16 > stx.size()) return false;
    uint32_t idx = 0;   // glyph index = set bits before the char
    for (uint32_t c = 0; c < 127 && idx < count; c++) {
        const size_t wp = b + bm + (c / 32) * 4;
        if (wp + 4 > stx.size()) return false;
        if (!(rd32(&stx[wp]) >> (c % 32) & 1)) continue;
        if (c >= 32) {
            const uint8_t* e = &stx[b + gt + size_t(idx) * 8];
            Glyph& q = f.g[c - 32];
            const uint32_t pk = uint32_t(e[1]) << 16 | uint32_t(e[2]) << 8 | e[3];
            q.w = e[0]; q.ax = int(pk & 0xFFF); q.ay = int(pk >> 12); q.h = e[4]; q.l = int8_t(e[5]); q.r = int8_t(e[6]); q.top = e[7];
            q.ok = q.ax + q.w <= tex.w && q.ay + q.h <= tex.h;
        }
        idx++;
    }
    f.atlas = tex;
    for (size_t i = 0; i + 3 < f.atlas.px.size(); i += 4) {   // luminance -> coverage, white
        const uint8_t a = std::max(f.atlas.px[i], std::max(f.atlas.px[i + 1], f.atlas.px[i + 2]));
        f.atlas.px[i] = f.atlas.px[i + 1] = f.atlas.px[i + 2] = 255; f.atlas.px[i + 3] = a;
    }
    f.cap = f.g['A' - 32].ok ? f.g['A' - 32].h : 0;
    return f.ok();
}

// ---- the set the menu uses --------------------------------------------------------------------------------------------------------
enum PadGlyph { PG_CROSS, PG_CIRCLE, PG_UP, PG_DOWN, PG_LEFT, PG_RIGHT, PG_N };
struct Art {
    Image pl[9];                                   // the pause panel 9-slice (TL, T, TR, L, C, R, BL, B, BR)
    Image cursor, cursorline, rule, band;          // the pause cursor sweep (additive), its glow line (additive), the header rule, the row band
    Image win[3], carsol, halftone, swirl;         // the general dialog: left, right, middle; the yellow button glow (additive); decorations
    Image guide[3];                                // the guide strip: left, middle, right
    Image pad[PG_N];                               // the game's button glyphs
    Font font;                                     // the system font (US)
    bool panel_ok() const { for (const Image& i : pl) if (!i.ok()) return false; return true; }
    bool dialog_ok() const { return win[0].ok() && win[1].ok() && win[2].ok(); }
    bool guide_ok() const { return guide[0].ok() && guide[1].ok() && guide[2].ok(); }
    bool pad_ok() const { for (const Image& i : pad) if (!i.ok()) return false; return true; }
    std::string summary() const
    {
        std::string s;
        auto add = [&s](const char* n, bool ok) { s += s.empty() ? "" : ", "; s += n; s += ok ? " yes" : " no"; };
        add("pause panel", panel_ok()); add("cursor", cursor.ok() && cursorline.ok()); add("dialog", dialog_ok() && carsol.ok());
        add("guide strip", guide_ok()); add("button glyphs", pad_ok()); add("font", font.ok());
        return s;
    }
    bool any() const { return panel_ok() || dialog_ok() || guide_ok() || pad_ok() || font.ok(); }
};

// The entry `path` ("Dir/File") of a CPK `base`, made plain.
inline bool cpk_get(jmods::Io& io, const jmods::Archive& a, const std::string& path, Bytes& out)
{
    const auto it = a.index.find(path);
    if (it == a.index.end()) return false;
    const jmods::TocRow& r = a.rows[it->second];
    if (r.size > (64u << 20)) return false;
    out.resize(size_t(r.size));
    if (!jmods::read_exact(io, a.base, a.toc_offset + r.offset, out.data(), out.size())) return false;
    return plain(out);
}
// The CPK files of one folder (the disc copy's PS3_GAME/USRDIR/data/PS3), read only, opened per read.
class DirIo : public jmods::Io {
public:
    explicit DirIo(std::string dir) : d_(std::move(dir)) {}
    bool mod_size(const std::string&, uint64_t&) override { return false; }
    size_t mod_read(const std::string&, uint64_t, void*, size_t) override { return 0; }
    bool archive_size(const std::string& b, uint64_t& n) override
    {
        FILE* f = std::fopen((d_ + "/" + b).c_str(), "rb");
        if (!f) return false;
        bool ok = seek(f, 0, SEEK_END);
        if (ok) n = tell(f);
        std::fclose(f);
        return ok;
    }
    size_t archive_read(const std::string& b, uint64_t off, void* dst, size_t n) override
    {
        FILE* f = std::fopen((d_ + "/" + b).c_str(), "rb");
        if (!f) return 0;
        const size_t got = seek(f, off, SEEK_SET) ? std::fread(dst, 1, n, f) : 0;
        std::fclose(f);
        return got;
    }
private:
#ifdef _WIN32
    static bool seek(FILE* f, uint64_t o, int w) { return _fseeki64(f, (long long)o, w) == 0; }
    static uint64_t tell(FILE* f) { return uint64_t(_ftelli64(f)); }
#else
    static bool seek(FILE* f, uint64_t o, int w) { return fseeko(f, (off_t)o, w) == 0; }
    static uint64_t tell(FILE* f) { return uint64_t(ftello(f)); }
#endif
    std::string d_;
};

// Reads every piece it can; `log` gets one line per failure. Slow part: two packages of a few MB decompressed (background thread).
inline Art load_art(jmods::Io& io, std::vector<std::string>& log)
{
    Art art;
    auto open = [&](const char* base, jmods::Archive& a) {
        a = jmods::load_archive(io, base);
        if (a.rows.empty()) { log.push_back(std::string(base) + ": " + (a.err.empty() ? "no entries" : a.err)); return false; }
        return true;
    };
    jmods::Archive game, battle;
    if (open("partition_op_game_ps3.cpk", game)) {
        Bytes am, av, um, uv, wm, wv, s, v;
        const bool all = cpk_get(io, game, "game/resident/game_resident_ALL_m.pak", am) && cpk_get(io, game, "game/resident/game_resident_ALL_v.pak", av);
        if (!all) log.push_back("game_resident_ALL: not read");
        if (all && stpk_get(am, "game_window_m.pak", wm) && stpk_get(av, "game_window_v.pak", wv)) {
            if (stpk_get(wm, "hanyou_window.srd", s) && stpk_get(wv, "hanyou_window.srdv", v)) {
                art.win[0] = srd_image(s, v, "window_000.png"); art.win[1] = srd_image(s, v, "window_001.png"); art.win[2] = srd_image(s, v, "window_002.png");
                art.carsol = srd_image(s, v, "carsol_000.png"); art.halftone = srd_image(s, v, "ef_000.png"); art.swirl = srd_image(s, v, "fire_000.png");
            } else log.push_back("hanyou_window: not found");
            if (stpk_get(wm, "guide.srd", s) && stpk_get(wv, "guide.srdv", v))
                for (int i = 0; i < 3; i++) art.guide[i] = srd_image(s, v, "main_guide_00" + std::to_string(i) + ".png");
            else log.push_back("guide: not found");
        } else if (all) log.push_back("game_window: not found");
        if (all && stpk_get(am, "pad_texture.srd", s) && stpk_get(av, "pad_texture.srdv", v)) {
            static const char* const n[PG_N] = { "05_sys_cross.png", "02_sys_circle.png", "20_sys_plus_up.png", "21_sys_plus_dn.png", "23_sys_plus_left.png", "22_sys_plus_right.png" };
            for (int i = 0; i < PG_N; i++) art.pad[i] = srd_image(s, v, n[i]);
        } else if (all) log.push_back("pad_texture: not found");
        if (cpk_get(io, game, "game/resident/game_resident_US_m.pak", um) && cpk_get(io, game, "game/resident/game_resident_US_v.pak", uv) &&
            stpk_get(um, "op_font_US_PS3.stx", s) && stpk_get(uv, "op_font_US_PS3.srdv", v)) {
            if (!spft_parse(s, srd_image(s, v, ""), art.font)) log.push_back("op_font_US_PS3: not decoded");
        } else log.push_back("op_font_US_PS3: not found");
    }
    if (open("partition_op_battle_ps3.cpk", battle)) {
        Bytes bm, bv, pm, pv, s, v;
        if (cpk_get(io, battle, "battle/resident/battle_resident_ALL_m.pak", bm) && cpk_get(io, battle, "battle/resident/battle_resident_ALL_v.pak", bv) &&
            stpk_get(bm, "pause_menu_ALL_m.pak", pm) && stpk_get(bv, "pause_menu_ALL_v.pak", pv) &&
            stpk_get(pm, "pause_menu_ALL_PS3.srd", s) && stpk_get(pv, "pause_menu_ALL_PS3.srdv", v)) {
            for (int i = 0; i < 9; i++) art.pl[i] = srd_image(s, v, "pause_pl_00" + std::to_string(i) + ".png");
            art.cursor = srd_image(s, v, "pause_cursor_000.png"); art.cursorline = srd_image(s, v, "pause_cursorline_000.png");
            art.rule = srd_image(s, v, "pause_line_000.png"); art.band = srd_image(s, v, "pause_gurade_000.png");
        } else log.push_back("pause_menu: not found");
    }
    return art;
}

}  // namespace jpart

#endif
