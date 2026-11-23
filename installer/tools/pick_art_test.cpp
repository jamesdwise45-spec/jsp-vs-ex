// Host test of the after-match menu's game-art reader and painter (installer/port/jsp/jsp_pick_art.h, jsp_pick_skin.h): the $CMP
// decoder (LZ, raw, Huffman chunks), STPK and SRD parsing, DXT1 / DXT5 / Morton ARGB decoding, the SpFt glyph table, and that every
// layout paints with no art at all (the fallback) inside its picture. Synthetic data only: no game files, no GPU.
//
// Desk use (NOT in CI, needs the player's own disc folder; writes nothing but the previews asked for):
//   pick_art_test --data <disc>/PS3_GAME/USRDIR/data/PS3 [--preview DIR] [--bg picture.ppm]
// reads the game's art exactly as the port does and paints the prompt, the picker, the Leave dialog and the band to DIR/*.ppm.
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
        size_t n = 0; for (const auto& t : f.text) n += t.s == "Fight again" || t.s == "Confirm"; CHECK(n == 2);
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
    const jpskin::Frame fb = jpskin::band(P, "Fight again: back to the room...");
    CHECK(inside(fb) && opaque(fb) > 0);
    const jpskin::Frame fl = jpskin::band(P, std::string(400, 'W'));   // a long band stays on screen
    CHECK(inside(fl));
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
static void preview(const std::string& out, const std::vector<jpskin::Frame>& fs, const std::vector<uint8_t>& bg, int bw, int bh)
{
    std::vector<float> img(size_t(1280) * 720 * 3, 0.12f);
    if (bw > 0) for (int y = 0; y < 720; y++) for (int x = 0; x < 1280; x++) for (int k = 0; k < 3; k++)
        img[(size_t(y) * 1280 + x) * 3 + k] = bg[(size_t(y * bh / 720) * bw + x * bw / 1280) * 3 + k] / 255.0f;
    for (const auto& f : fs)
        for (int y = 0; y < f.ph; y++) for (int x = 0; x < f.pw; x++) {
            const uint8_t* s = &f.pic[(size_t(y) * f.pw + x) * 4]; const int X = f.x + x, Y = f.y + y;
            if (X < 0 || Y < 0 || X >= 1280 || Y >= 720) continue;
            const float a = s[3] / 255.0f;
            for (int k = 0; k < 3; k++) { float& d = img[(size_t(Y) * 1280 + X) * 3 + k]; d = s[k] / 255.0f * a + d * (1 - a); }
        }
    FILE* o = std::fopen(out.c_str(), "wb"); if (!o) return;
    std::fprintf(o, "P6 1280 720 255\n");
    for (float v : img) std::fputc(int(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f), o);
    std::fclose(o);
    for (const auto& f : fs) for (const auto& t : f.text) std::printf("  overlay text at %.0f,%.0f scale %.2f: %s\n", t.x, t.y, t.scale, t.s.c_str());
}
static int desk(const char* data, const char* dir, const char* bgp)
{
    jpart::DirIo io(data);
    std::vector<std::string> log;
    const jpart::Art art = jpart::load_art(io, log);
    for (const auto& l : log) std::printf("[pickui] %s\n", l.c_str());
    std::printf("[pickui] game art: %s\n", art.summary().c_str());
    if (!dir) return art.any() ? 0 : 1;
    int bw = 0, bh = 0; const std::vector<uint8_t> bg = bgp ? read_ppm(bgp, bw, bh) : std::vector<uint8_t>();
    const jpskin::Painter P(&art, nullptr);
    jpskin::View v = prompt_view();
    { const auto t0 = std::chrono::steady_clock::now(); for (int i = 0; i < 10; i++) (void)jpskin::menu(P, v, 1.0f, 1.0f);
      std::printf("[pickui] menu paint: %.2f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10); }
    preview(std::string(dir) + "/prompt.ppm", { jpskin::menu(P, v, 1.0f, 1.0f) }, bg, bw, bh);
    v.sel = 0; preview(std::string(dir) + "/prompt_sel0_open50.ppm", { jpskin::menu(P, v, 0.5f, 0.4f) }, bg, bw, bh);
    jpskin::View p; p.kind = jpskin::MENU; p.title = "Select character"; p.sel = 1;
    p.rows = { { "Battle Character", "Gintoki Sakata" }, { "Support Character", "Random" }, { "Teams", "1st + 3rd to join vs 2nd + COM" } };
    p.lines = { { "Pick your character", jpskin::WHITE }, { "You: Gintoki Sakata / Random", jpskin::GREY }, { "Opponent: choosing...", jpskin::GREY }, { "Your pick locks in 21 s", jpskin::AMBER } };
    p.guide = { { jpart::PG_LEFT, jpart::PG_RIGHT, "Change" }, { jpart::PG_CROSS, -1, "Ready" } };
    preview(std::string(dir) + "/picker.ppm", { jpskin::menu(P, p, 1.0f, 1.0f) }, bg, bw, bh);
    jpskin::View d; d.kind = jpskin::DIALOG; d.title = "Leave the match?"; d.rows = { { "Leave", "" } };
    d.lines = { { "Rollback ends for you.", jpskin::GREY }, { "Then leave the room with Circle.", jpskin::GREY } };
    d.guide = { { jpart::PG_CROSS, -1, "Leave" }, { jpart::PG_CIRCLE, -1, "Back" } };
    preview(std::string(dir) + "/leave.ppm", { jpskin::dialog(P, &art, d, 1.0f) }, bg, bw, bh);
    preview(std::string(dir) + "/band.ppm", { jpskin::band(P, "Fight again: back to the room...") }, bg, bw, bh);
    const jpskin::Painter F(nullptr, nullptr);
    preview(std::string(dir) + "/fallback_prompt.ppm", { jpskin::menu(F, prompt_view(), 1.0f, 1.0f) }, bg, bw, bh);
    return art.any() ? 0 : 1;
}

int main(int argc, char** argv)
{
    const char *data = nullptr, *dir = nullptr, *bg = nullptr;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--data")) data = argv[i + 1];
        else if (!std::strcmp(argv[i], "--preview")) dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--bg")) bg = argv[i + 1];
    }
    if (data) return desk(data, dir, bg);
    test_cmp(); test_stpk_srd(); test_font(); test_fallback();
    if (fails) { std::printf("pick_art_test: %d failure(s)\n", fails); return 1; }
    std::printf("pick_art_test: all passed\n");
    return 0;
}
