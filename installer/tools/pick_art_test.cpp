// Host test of the after-match menu's game-art reader and painter (installer/port/jsp/jsp_pick_art.h, jsp_pick_skin.h): the $CMP
// decoder (LZ, raw, Huffman chunks), STPK and SRD parsing, DXT1 / DXT5 / Morton ARGB decoding, the SpFt glyph table, and that every
// layout paints with no art at all (the fallback) inside its picture. Synthetic data only: no game files, no GPU.
//
// Desk use (NOT in CI, needs the player's own disc folder; writes nothing but the previews asked for):
//   pick_art_test --data <disc>/PS3_GAME/USRDIR/data/PS3 [--preview DIR] [--bg picture.ppm] [--font menu_font.jmf]
// reads the game's art exactly as the port does and paints the prompt, the picker, the Leave dialog and the band to DIR/*.ppm (with
// --font, the overlay-font texts too: the header, and every text of the fallback).
#include "../port/jsp/jsp_pick_skin.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
using jpart::Bytes;

static void put32(Bytes& b, uint32_t v) { b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
static void put16(Bytes& b, uint32_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
static void tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }
static void pad16(Bytes& b) { while (b.size() % 16) b.push_back(0); }

// ---- $CMP -----------------------------------------------------------------------------------------------------------------------------
static Bytes cmp_file(const std::vector<Bytes>& chunks, uint32_t total)
{
    Bytes f; tag(f, "$CMP"); put32(f, 0); put32(f, 0); put32(f, 0); put32(f, total); put32(f, 0); put32(f, 0); put32(f, 0);
    for (const Bytes& c : chunks) f.insert(f.end(), c.begin(), c.end());
    return f;
}
static Bytes chunk(const char* t, uint32_t unpacked, const Bytes& body)
{
    Bytes c; tag(c, t); put32(c, unpacked); put32(c, uint32_t(body.size() + 16)); put32(c, 0); c.insert(c.end(), body.begin(), body.end()); return c;
}
// a $CH0 block coding `data` with fixed 8-bit codes for the symbols present (code = symbol)
static Bytes huff_block(const Bytes& data, bool one = false)
{
    Bytes syms; for (int s = 0; s < 256; s++) for (uint8_t d : data) if (d == s) { syms.push_back(uint8_t(s)); break; }
    Bytes t;
    if (one) { t.push_back(0); t.push_back(0); t.push_back(0); t.push_back(data[0]); }
    else for (uint8_t s : syms) { t.push_back(s); t.push_back(0); t.push_back(8 << 3); t.push_back(s); }
    Bytes bits = one ? Bytes() : data;
    Bytes h; tag(h, "$CH0"); put32(h, uint32_t(data.size())); put32(h, uint32_t(16 + t.size() + bits.size())); put32(h, one ? 1 : uint32_t(syms.size()));
    h.insert(h.end(), t.begin(), t.end()); h.insert(h.end(), bits.begin(), bits.end());
    return h;
}
static void test_cmp()
{
    // $CL0: literal "abc", then copy 6 from distance 3 -> "abcabcabc"
    Bytes lz = { uint8_t(3 << 1), 'a', 'b', 'c', uint8_t(1 | (6 << 1)), 3 };
    Bytes f = cmp_file({ chunk("$CL0", 9, lz), chunk("$CR0", 2, Bytes{ 'x', 'y' }) }, 11), out;
    CHECK(jpart::cmp_decode(f.data(), f.size(), out));
    CHECK(std::string(out.begin(), out.end()) == "abcabcabcxy");
    // a distance past the chunk start is refused
    Bytes bad = { uint8_t(1 | (2 << 1)), 1 };
    Bytes fb = cmp_file({ chunk("$CR0", 2, Bytes{ 'q', 'r' }), chunk("$CL0", 2, bad) }, 4);
    CHECK(!jpart::cmp_decode(fb.data(), fb.size(), out));
    // $CH0 and $CLH (a Huffman-coded $CL0 chunk)
    Bytes raw = { 'h', 'e', 'l', 'l', 'o' };
    Bytes ch = huff_block(raw);
    Bytes fc = cmp_file({ ch }, 5);
    // the $CH0 chunk's own header is its 16-byte block header: its "size" field must cover the whole block
    CHECK(jpart::cmp_decode(fc.data(), fc.size(), out) && std::string(out.begin(), out.end()) == "hello");
    Bytes inner = chunk("$CL0", 9, lz), hb = huff_block(inner);
    Bytes fh = cmp_file({ chunk("$CLH", 9, hb) }, 9);
    CHECK(jpart::cmp_decode(fh.data(), fh.size(), out) && std::string(out.begin(), out.end()) == "abcabcabc");
    Bytes one = huff_block(Bytes(7, 'z'), true);
    Bytes fo = cmp_file({ one }, 7);
    CHECK(jpart::cmp_decode(fo.data(), fo.size(), out) && out == Bytes(7, 'z'));
    // wrong totals, truncation, not a $CMP
    Bytes ft = cmp_file({ chunk("$CR0", 2, Bytes{ 'x', 'y' }) }, 3);
    CHECK(!jpart::cmp_decode(ft.data(), ft.size(), out));
    CHECK(!jpart::cmp_decode(f.data(), f.size() - 1, out));
    Bytes p = { 'S', 'T', 'P', 'K' }; CHECK(jpart::plain(p) && p.size() == 4);
}

// ---- STPK / SRD ----------------------------------------------------------------------------------------------------------------------
static Bytes stpk(const std::vector<std::pair<std::string, Bytes>>& es)
{
    Bytes b; tag(b, "STPK"); put32(b, 1); put32(b, uint32_t(es.size())); put32(b, 0x10);
    uint32_t off = uint32_t(0x10 + es.size() * 0x30);
    for (const auto& e : es) {
        put32(b, off); put32(b, uint32_t(e.second.size())); put32(b, 0); put32(b, 0);
        char n[32] = {}; std::memcpy(n, e.first.data(), std::min<size_t>(32, e.first.size())); b.insert(b.end(), n, n + 32);
        off += uint32_t(e.second.size());
    }
    for (const auto& e : es) b.insert(b.end(), e.second.begin(), e.second.end());
    return b;
}
static void srd_tex(Bytes& s, const char* name, int w, int h, int fmt, uint32_t off, uint32_t len)
{
    Bytes d; put32(d, 1); put16(d, 0x0300); put16(d, uint32_t(w)); put16(d, uint32_t(h)); put16(d, uint32_t(w)); d.push_back(uint8_t(fmt)); d.push_back(1); d.push_back(0); d.push_back(0);
    Bytes r; r.assign(16, 0); r[0] = 7; put32(r, 0x40000000u | off); put32(r, len); put32(r, 0x80); put32(r, 0);
    r.insert(r.end(), name, name + std::strlen(name) + 1);
    Bytes sub; tag(sub, "$RSI"); put32(sub, uint32_t(r.size())); put32(sub, 0); put32(sub, 0); sub.insert(sub.end(), r.begin(), r.end()); pad16(sub);
    tag(s, "$TXR"); put32(s, uint32_t(d.size())); put32(s, uint32_t(sub.size())); put32(s, 0); s.insert(s.end(), d.begin(), d.end()); pad16(s);
    s.insert(s.end(), sub.begin(), sub.end());
}
static void test_stpk_srd()
{
    const std::string longname = "game_resident_character_param_PS3.dat";   // cut at 32 in the package
    Bytes pk = stpk({ { "a.srd", Bytes{ 1, 2, 3 } }, { longname, Bytes{ 9 } } }), out;
    CHECK(jpart::stpk_get(pk, "a.srd", out) && out == (Bytes{ 1, 2, 3 }));
    CHECK(jpart::stpk_get(pk, longname, out) && out == Bytes{ 9 });
    CHECK(!jpart::stpk_get(pk, "b.srd", out));
    Bytes s; tag(s, "$CFH"); put32(s, 0); put32(s, 0); put32(s, 1);
    srd_tex(s, "one.png", 4, 4, 0x0F, 0, 8);
    srd_tex(s, "two.png", 4, 4, 0x11, 8, 16);
    tag(s, "$CT0"); put32(s, 0); put32(s, 0); put32(s, 0);
    const auto t = jpart::srd_textures(s);
    CHECK(t.size() == 2 && t[0].name == "one.png" && t[0].w == 4 && t[0].fmt == 0x0F && t[1].off == 8 && t[1].len == 16 && t[1].fmt == 0x11);
    // DXT1 block: colour 0 = white, colour 1 = black, indices: row 0 all 0, row 1 all 1, row 2 all 2, row 3 all 3 (c0 > c1: 4 colours)
    Bytes v = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x55, 0xAA, 0xFF };
    // DXT5: alpha 255 / 0, indices 0 for row 0 and 1 for the rest; colour all white
    Bytes a5 = { 255, 0, 0x00, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0, 0, 0, 0 };
    a5[2] = 0; a5[3] = 0x90; a5[4] = 0x24; a5[5] = 0x49; a5[6] = 0x92; a5[7] = 0x24;   // idx 0 x4 then idx 1 x12 (3 bits each)
    v.insert(v.end(), a5.begin(), a5.end());
    const jpart::Image i1 = jpart::srd_image(s, v, "one.png"), i2 = jpart::srd_image(s, v, "two.png");
    CHECK(i1.ok() && i1.px[0] == 255 && i1.px[3] == 255 && i1.px[4 * 4] == 0 && i1.px[8 * 4] == 170 && i1.px[12 * 4] == 85);
    CHECK(i2.ok() && i2.px[3] == 255 && i2.px[(4 + 1) * 4 + 3] == 0);
    CHECK(!jpart::srd_image(s, Bytes(4), "two.png").ok());   // short pixel data
    // Morton ARGB 2x2: memory order (0,0) (1,0) (0,1) (1,1)
    Bytes m = { 255, 1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 128, 4, 0, 0 };
    const jpart::Image im = jpart::decode_argb_morton(m.data(), m.size(), 2, 2);
    CHECK(im.ok() && im.px[0] == 1 && im.px[4] == 2 && im.px[8] == 3 && im.px[12] == 4 && im.px[15] == 128);
    CHECK(!jpart::decode_argb_morton(m.data(), m.size(), 3, 2).ok());
}

// ---- SpFt -------------------------------------------------------------------------------------------------------------------------------
static void test_font()
{
    // chars 32..126 present (bits 32..126), glyph i at atlas x = 2 i, w 2, y 1, h 3, left -1, right 1, top 5
    Bytes s(0x40, 0); std::memcpy(s.data(), "SpFt", 4);
    const uint32_t gt = 0x40 + 16, bm = 0x40;
    auto w32 = [&s](size_t at, uint32_t v) { s[at] = uint8_t(v >> 24); s[at + 1] = uint8_t(v >> 16); s[at + 2] = uint8_t(v >> 8); s[at + 3] = uint8_t(v); };
    w32(0x14, 95); w32(0x18, gt); w32(0x1C, bm); s[0x24] = 46; s[0x26] = 37;
    s.resize(gt + 95 * 8, 0);
    w32(bm, 0); w32(bm + 4, 0xFFFFFFFFu); w32(bm + 8, 0xFFFFFFFFu); w32(bm + 12, 0x7FFFFFFFu);
    for (int i = 0; i < 95; i++) {
        uint8_t* e = &s[gt + size_t(i) * 8];
        const uint32_t pk = (1u << 12) | uint32_t(2 * i);
        e[0] = 2; e[1] = uint8_t(pk >> 16); e[2] = uint8_t(pk >> 8); e[3] = uint8_t(pk); e[4] = 3; e[5] = 0xFF; e[6] = 1; e[7] = 5;
    }
    jpart::Image tex; tex.w = 256; tex.h = 8; tex.px.assign(size_t(256) * 8 * 4, 0);
    for (size_t i = 0; i < tex.px.size(); i += 4) { tex.px[i] = 200; tex.px[i + 3] = 255; }
    jpart::Font f;
    CHECK(jpart::spft_parse(s, tex, f));
    CHECK(f.line == 46 && f.base == 37 && f.cap == 3);
    CHECK(f.g['A' - 32].ax == 2 * 33 && f.g['A' - 32].ay == 1 && f.g['A' - 32].l == -1 && f.g['~' - 32].ok);
    CHECK(f.atlas.px[3] == 200 && f.atlas.px[0] == 255);   // luminance became coverage
    CHECK(f.width("AB", 1.0f) == 4.0f && f.width("AB", 2.0f) == 8.0f);
    Bytes cut(s.begin(), s.begin() + 0x30);
    CHECK(!jpart::spft_parse(cut, tex, f));
}

// ---- the painter with no art (the fallback) ---------------------------------------------------------------------------------------------
static bool inside(const jpskin::Frame& f) { return f.x >= 0 && f.y >= 0 && f.x + f.pw <= 1280 && f.y + f.ph <= 720 && f.pic.size() == size_t(f.pw) * f.ph * 4; }
static size_t opaque(const jpskin::Frame& f) { size_t n = 0; for (size_t i = 3; i < f.pic.size(); i += 4) n += f.pic[i] > 0; return n; }
static jpskin::View prompt_view()
{
    jpskin::View v; v.kind = jpskin::MENU; v.title = "Match over"; v.rows = { { "Fight again", "" }, { "Select character", "" } }; v.sel = 1;
    v.lines = { { "Choose what to do next", jpskin::WHITE }, { "Opponent wants a rematch", jpskin::GREY }, { "Returning to lobby in 12 s", jpskin::AMBER } };
    v.guide = { { jpart::PG_CROSS, -1, "Confirm" }, { jpart::PG_CIRCLE, -1, "Leave" } };
    return v;
}
static void test_fallback()
{
    const jpskin::Painter P(nullptr, [](const std::string& s, float sc) { return float(s.size()) * 15.0f * sc; });
    const jpskin::View v = prompt_view();
    for (float open : { 0.0f, 0.5f, 1.0f }) {
        const jpskin::Frame f = jpskin::menu(P, v, open, 1.0f);
        CHECK(inside(f) && f.pw >= jpskin::kMenuW);
        CHECK(open == 0.0f ? opaque(f) == 0 : opaque(f) > size_t(f.pw) * size_t(f.ph) / 2);
        CHECK(!f.text.empty() && f.text[0].s == "Match over");   // the header in the overlay font, then the body texts (no game font)
        size_t n = 0; for (const auto& t : f.text) n += t.s == "Fight again"; CHECK(n == 1);   // the guide is its own bar now
    }
    jpskin::View p; p.kind = jpskin::MENU; p.title = "Select character"; p.sel = 0;
    p.rows = { { "Battle Character", "Naruto Uzumaki" }, { "Support Character", "Random" }, { "Teams", "versus, each with a COM" } };
    for (int i = 0; i < 6; i++) p.lines.push_back({ "line", jpskin::WHITE });
    const jpskin::Frame fp = jpskin::menu(P, p, 1.0f, 0.3f);
    CHECK(inside(fp));
    jpskin::View d; d.kind = jpskin::DIALOG; d.title = "Leave the match?"; d.rows = { { "Leave", "" } }; d.lines = { { "x", jpskin::GREY } };
    d.guide = { { jpart::PG_CROSS, -1, "Leave" }, { jpart::PG_CIRCLE, -1, "Back" } };
    const jpskin::Frame fd = jpskin::dialog(P, nullptr, d, 1.0f);
    CHECK(inside(fd) && opaque(fd) > 0);
    const jpskin::Frame fb = jpskin::band(P, nullptr, "Fight again: back to the room...");
    CHECK(inside(fb) && opaque(fb) > 0);
    const jpskin::Frame fl = jpskin::band(P, nullptr, std::string(400, 'W'));   // a long band stays on screen
    CHECK(inside(fl));
    const jpskin::Frame fg = jpskin::guide_bar(P, v.guide);   // the guide bar at the game's place, above nothing else of ours
    CHECK(inside(fg) && opaque(fg) > 0 && fg.y >= fb.y + fb.ph);
    for (const jpskin::Frame& m : { jpskin::menu(P, v, 1.0f, 1.0f), fp, fd }) CHECK(m.y + m.ph <= fg.y);   // panels end above the guide bar
    CHECK(v.key() != p.key() && v.key() == prompt_view().key());
}

// ---- desk: the real art ---------------------------------------------------------------------------------------------------------------
static std::vector<uint8_t> read_ppm(const char* p, int& w, int& h)
{
    std::vector<uint8_t> px; FILE* f = std::fopen(p, "rb"); w = h = 0;
    if (!f) return px;
    int mx = 0;
    if (std::fscanf(f, "P6 %d %d %d", &w, &h, &mx) == 3 && w > 0 && h > 0) { std::fgetc(f); px.resize(size_t(w) * h * 3); if (std::fread(px.data(), 1, px.size(), f) != px.size()) px.clear(); }
    std::fclose(f); return px;
}
// The overlay's own font (menu_font.jmf, the port's art: installer/tools/menu_font_art.py), drawn as the overlay does, so the previews show the
// header and the fallback texts too. Layout as in menu_font_art.py: 10 u32 LE header, count x 8 u16 LE glyph records, RGBA atlas.
struct Jmf {
    jpart::Image atlas; uint32_t ns = 0, first = 0, count = 0, cell_h = 0, base = 0, stride = 0, cap = 0; std::vector<uint16_t> tab;
    bool load(const char* path)
    {
        FILE* f = path ? std::fopen(path, "rb") : nullptr; if (!f) return false;
        uint32_t h[10] = {};
        bool ok = std::fread(h, 4, 10, f) == 10 && h[0] == 0x31464D4Au;
        if (ok) { ns = h[3]; first = h[4]; count = h[5]; cell_h = h[6]; base = h[7]; stride = h[8]; cap = h[9]; tab.resize(size_t(count) * 8);
                  atlas.w = int(h[1]); atlas.h = int(h[2]); atlas.px.resize(size_t(h[1]) * h[2] * 4);
                  ok = std::fread(tab.data(), 2, tab.size(), f) == tab.size() && std::fread(atlas.px.data(), 1, atlas.px.size(), f) == atlas.px.size(); }
        std::fclose(f); return ok;
    }
    float width(const std::string& s, float scale) const
    {
        float w = 0;
        for (unsigned char ch : s) { const uint32_t i = ch >= first && ch < first + count ? ch - first : '?' - first; w += tab[size_t(i) * 8 + 4] / 16.0f; }
        return w * scale;
    }
    void draw(jpskin::Canvas& c, const jpskin::TextOp& t) const
    {
        const float by = t.y + cap * t.scale;
        const int k = t.style < 0 ? 0 : t.style;
        const int planes[2] = { ns >= 6 && k < 2 ? 2 + 2 * k : k, ns >= 6 && k < 2 ? 3 + 2 * k : -1 };
        const float r = (t.rgba & 255) / 255.0f, g = (t.rgba >> 8 & 255) / 255.0f, b = (t.rgba >> 16 & 255) / 255.0f, a = (t.rgba >> 24) / 255.0f;
        for (int pl : planes) {
            if (pl < 0) continue;
            float pen = t.x;
            for (unsigned char ch : t.s) {
                uint32_t i = ch >= first && ch < first + count ? ch - first : '?' - first;
                const uint16_t* m = &tab[size_t(i) * 8];
                if (m[2]) c.image(atlas, m[0], float(m[1] + pl * stride), m[2], float(cell_h), pen - int16_t(m[3]) * t.scale, by - base * t.scale,
                                  m[2] * t.scale, cell_h * t.scale, a, jpskin::OVER, r, g, b);
                pen += m[4] / 16.0f * t.scale;
            }
        }
    }
};
static Jmf g_jmf; static bool g_jmf_ok = false;
static void preview(const std::string& out, const std::vector<jpskin::Frame>& fs, const std::vector<uint8_t>& bg, int bw, int bh)
{
    jpskin::Canvas c; c.make(1280, 720);
    for (int y = 0; y < 720; y++) for (int x = 0; x < 1280; x++) {
        float v[3] = { 0.12f, 0.12f, 0.12f };
        if (bw > 0) for (int k = 0; k < 3; k++) v[k] = bg[(size_t(y * bh / 720) * bw + x * bw / 1280) * 3 + k] / 255.0f;
        c.over(x, y, v[0], v[1], v[2], 1.0f);
    }
    for (const auto& f : fs) {
        jpart::Image im; im.w = f.pw; im.h = f.ph; im.px = f.pic;
        c.image(im, float(f.x), float(f.y), float(f.pw), float(f.ph), 1.0f);
        for (const auto& t : f.text) { if (g_jmf_ok) g_jmf.draw(c, t); else std::printf("  overlay text (no --font) at %.0f,%.0f: %s\n", t.x, t.y, t.s.c_str()); }
    }
    std::vector<uint8_t> px; c.straight(px);
    FILE* o = std::fopen(out.c_str(), "wb"); if (!o) return;
    std::fprintf(o, "P6 1280 720 255\n");
    for (size_t i = 0; i < px.size(); i += 4) { std::fputc(px[i], o); std::fputc(px[i + 1], o); std::fputc(px[i + 2], o); }
    std::fclose(o);
}
static int desk(const char* data, const char* dir, const char* bgp, const char* font)
{
    g_jmf_ok = g_jmf.load(font);
    jpart::DirIo io(data);
    std::vector<std::string> log;
    const jpart::Art art = jpart::load_art(io, log);
    for (const auto& l : log) std::printf("[pickui] %s\n", l.c_str());
    std::printf("[pickui] game art: %s\n", art.summary().c_str());
    if (!dir) return art.any() ? 0 : 1;
    int bw = 0, bh = 0; const std::vector<uint8_t> bg = bgp ? read_ppm(bgp, bw, bh) : std::vector<uint8_t>();
    const jpskin::Painter P(&art, nullptr);
    jpskin::View v = prompt_view();
    v.rows = { { art.word("PAU_011", "Fight again"), "" }, { art.word("PAU_009", "Re-select character(s)"), "" } };   // the glue's words
    v.guide = { { jpart::PG_CROSS, -1, art.word("BUT_000", "Confirm") }, { jpart::PG_CIRCLE, -1, art.word("BUT_017", "Leave") } };
    for (const auto& kv : art.words) std::printf("[pickui] word %s = \"%s\"\n", kv.first.c_str(), kv.second.c_str());
    { const auto t0 = std::chrono::steady_clock::now(); for (int i = 0; i < 10; i++) (void)jpskin::menu(P, v, 1.0f, 1.0f);
      std::printf("[pickui] menu paint: %.2f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10); }
    const std::string d0 = std::string(dir) + "/";
    preview(d0 + "prompt.ppm", { jpskin::menu(P, v, 1.0f, 1.0f), jpskin::guide_bar(P, v.guide) }, bg, bw, bh);
    v.sel = 0;
    { jpskin::Frame half = jpskin::menu(P, v, 1.0f, 1.0f), gb = jpskin::guide_bar(P, v.guide);   // the fade at 50 % as the glue blits it (alpha scaled, 6 px lift)
      for (jpskin::Frame* f : { &half, &gb }) { for (size_t i = 3; i < f->pic.size(); i += 4) f->pic[i] = uint8_t(f->pic[i] / 2); for (auto& t : f->text) t.rgba = (t.rgba & 0xFFFFFFu) | (t.rgba >> 25) << 24; }
      half.y += 6; preview(d0 + "prompt_sel0_open50.ppm", { half, gb }, bg, bw, bh); }
    jpskin::View p; p.kind = jpskin::MENU; p.title = "Select character"; p.sel = 1;
    p.rows = { { "Battle Character", "Gintoki Sakata" }, { "Support Character", "Random" }, { "Teams", "P1 + COM vs P2 + COM" } };
    p.lines = { { "Pick your character", jpskin::WHITE }, { "P1 (you): Gintoki Sakata + Random, choosing", jpskin::GREY }, { "P2: Random + Random, choosing", jpskin::GREY }, { "Your pick locks in 21 s", jpskin::AMBER } };
    p.guide = { { jpart::PG_LEFT, jpart::PG_RIGHT, "Change" }, { jpart::PG_CROSS, -1, "Ready" } };
    preview(d0 + "picker.ppm", { jpskin::menu(P, p, 1.0f, 1.0f), jpskin::guide_bar(P, p.guide) }, bg, bw, bh);
    jpskin::View d; d.kind = jpskin::DIALOG; d.title = "Leave the match?"; d.rows = { { art.word("BUT_017", "Leave"), "" } };
    d.lines = { { "Rollback ends for you.", jpskin::GREY }, { "Then leave the room with Circle.", jpskin::GREY } };
    d.guide = { { jpart::PG_CROSS, -1, art.word("BUT_017", "Leave") }, { jpart::PG_CIRCLE, -1, art.word("BUT_013", "Back") } };
    preview(d0 + "leave.ppm", { jpskin::dialog(P, &art, d, 1.0f), jpskin::guide_bar(P, d.guide) }, bg, bw, bh);
    preview(d0 + "band.ppm", { jpskin::band(P, &art, art.word("PAU_011", "Fight again") + ": back to the room...") }, bg, bw, bh);
    // the game's own end-of-fight (Free Battle finish) menu as its art and tables describe it: its "Pause Menu" title plate and its rows
    // (table 004: Fight again, Re-select character(s), ... ; layout as ours: its real placement lives in the game's code, not in its data)
    { jpskin::View r; r.kind = jpskin::MENU; r.title = ""; r.sel = 0;
      r.rows = { { art.word("PAU_011", "Fight again"), "" }, { art.word("PAU_009", "Re-select character(s)"), "" }, { "View replay", "" }, { "Save Replay", "" }, { "Return to main menu", "" } };
      jpskin::Frame f = jpskin::menu(P, r, 1.0f, 1.0f), plate;
      if (art.title_plate.ok()) { plate.x = f.x + 20; plate.y = f.y + 9;   /* where our header sits */ plate.pw = art.title_plate.w; plate.ph = art.title_plate.h; plate.pic = art.title_plate.px; }
      preview(d0 + "game_endmenu_recon.ppm", { f, plate, jpskin::guide_bar(P, v.guide) }, bg, bw, bh); }
    const jpskin::Painter F(nullptr, [](const std::string& t, float sc) { return g_jmf_ok ? g_jmf.width(t, sc) : 0.0f; });
    preview(d0 + "fallback_prompt.ppm", { jpskin::menu(F, prompt_view(), 1.0f, 1.0f), jpskin::guide_bar(F, prompt_view().guide) }, bg, bw, bh);
    preview(d0 + "fallback_band.ppm", { jpskin::band(F, nullptr, "Fight again: back to the room...") }, bg, bw, bh);
    return art.any() ? 0 : 1;
}

int main(int argc, char** argv)
{
    const char *data = nullptr, *dir = nullptr, *bg = nullptr, *font = nullptr;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--data")) data = argv[i + 1];
        else if (!std::strcmp(argv[i], "--preview")) dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--bg")) bg = argv[i + 1];
        else if (!std::strcmp(argv[i], "--font")) font = argv[i + 1];
    }
    if (data) return desk(data, dir, bg, font);
    test_cmp(); test_stpk_srd(); test_font(); test_fallback();
    if (fails) { std::printf("pick_art_test: %d failure(s)\n", fails); return 1; }
    std::printf("pick_art_test: all passed\n");
    return 0;
}
