// ENHANCEMENT (after-match menu in the game's own look, JSP_ROLLBACK_PICK=1): lays out and paints the online after-match prompt, its
// Leave confirm, the Select character picker and the one-line band with the game's own art (jsp_pick_art.h), into an RGBA picture the
// overlay blits (jsp_overlay_blit_rgba) plus a few header texts in the overlay's menu-label font. Header only and pure (no Windows,
// toolkit or game calls); host-tested by installer/tools/pick_art_test.cpp. Design: D:\jsp-audit\pick-ui\ART-REVIEW.md.
//
// Every piece has a drawn fallback in the same palette, used when the player's game data did not give it (art == nullptr or the piece
// missing): black glass panel with a light rim, a gold gradient cursor, drawn circle glyphs, and the overlay font for the text.
// Layout space: 1280 x 720 (the overlay stretches it). Motion: `open` 0..1 fades the frame in and lifts it 12 px; `sweep` 0..1 grows the
// cursor's gold sweep after a move.
#ifndef JSP_PICK_SKIN_H
#define JSP_PICK_SKIN_H

#include "jsp_pick_art.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jpskin {

using jpart::Image;
enum Mode { OVER, GLOW };   // GLOW: an additive picture (black = nothing) drawn as light over the frame

// Premultiplied float RGBA picture.
struct Canvas {
    int w = 0, h = 0;
    std::vector<float> c;
    void make(int W, int H) { w = std::max(W, 1); h = std::max(H, 1); c.assign(size_t(w) * size_t(h) * 4, 0.0f); }
    void over(int x, int y, float r, float g, float b, float a)   // premultiplied source
    {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) return;
        float* d = &c[(size_t(y) * w + x) * 4];
        const float k = 1.0f - std::min(a, 1.0f);
        d[0] = r + d[0] * k; d[1] = g + d[1] * k; d[2] = b + d[2] * k; d[3] = std::min(a, 1.0f) + d[3] * k;
    }
    void fill(float x, float y, float W, float H, uint8_t r, uint8_t g, uint8_t b, float a)
    {
        for (int yy = int(std::floor(y)); yy < int(std::ceil(y + H)); yy++)
            for (int xx = int(std::floor(x)); xx < int(std::ceil(x + W)); xx++) over(xx, yy, r / 255.0f * a, g / 255.0f * a, b / 255.0f * a, a);
    }
    // a horizontal gradient of one colour, alpha a0 at the left to a1 at the right
    void hgrad(float x, float y, float W, float H, uint8_t r, uint8_t g, uint8_t b, float a0, float a1)
    {
        for (int xx = int(x); xx < int(x + W); xx++) {
            const float t = W > 1 ? (xx - x) / (W - 1) : 0.0f, a = a0 + (a1 - a0) * t;
            for (int yy = int(y); yy < int(y + H); yy++) over(xx, yy, r / 255.0f * a, g / 255.0f * a, b / 255.0f * a, a);
        }
    }
    // src rect of `im` stretched onto the dst rect (bilinear, clamped inside the src rect); tint multiplies the colour
    void image(const Image& im, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh, float alpha, Mode mode = OVER,
               float tr = 1, float tg = 1, float tb = 1)
    {
        if (!im.ok() || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || alpha <= 0) return;
        const int x0 = std::max(0, int(std::floor(dx))), x1 = std::min(w, int(std::ceil(dx + dw)));
        const int y0 = std::max(0, int(std::floor(dy))), y1 = std::min(h, int(std::ceil(dy + dh)));
        auto px = [&im](int x, int y) { return &im.px[(size_t(y) * im.w + x) * 4]; };
        const float lo_x = sx, hi_x = sx + sw - 1, lo_y = sy, hi_y = sy + sh - 1;
        for (int y = y0; y < y1; y++) {
            const float v = std::clamp(sy + (y + 0.5f - dy) / dh * sh - 0.5f, lo_y, hi_y);
            const int iy = std::min(int(v), im.h - 1), jy = std::min(iy + 1, std::min(int(hi_y), im.h - 1)); const float fy = v - iy;
            for (int x = x0; x < x1; x++) {
                const float u = std::clamp(sx + (x + 0.5f - dx) / dw * sw - 0.5f, lo_x, hi_x);
                const int ix = std::min(int(u), im.w - 1), jx = std::min(ix + 1, std::min(int(hi_x), im.w - 1)); const float fx = u - ix;
                float s[4];
                for (int k = 0; k < 4; k++) {   // bilinear on premultiplied values
                    auto pm = [k](const uint8_t* p) { return k == 3 ? p[3] / 255.0f : p[k] / 255.0f * (p[3] / 255.0f); };
                    const float a = pm(px(ix, iy)) * (1 - fx) + pm(px(jx, iy)) * fx, b = pm(px(ix, jy)) * (1 - fx) + pm(px(jx, jy)) * fx;
                    s[k] = a * (1 - fy) + b * fy;
                }
                if (mode == GLOW) { s[3] = std::max(s[0], std::max(s[1], s[2])); }
                over(x, y, s[0] * tr * alpha, s[1] * tg * alpha, s[2] * tb * alpha, s[3] * alpha);
            }
        }
    }
    void image(const Image& im, float dx, float dy, float dw, float dh, float alpha, Mode mode = OVER) { image(im, 0, 0, float(im.w), float(im.h), dx, dy, dw, dh, alpha, mode); }
    // text in the game's font; (x, baseline) in this canvas; colour 0..255
    float text(const jpart::Font& f, float x, float base, const std::string& s, float scale, uint8_t r, uint8_t g, uint8_t b, float alpha)
    {
        const float top = base - f.base * scale;
        for (unsigned char ch : s) {
            const jpart::Glyph& q = f.g[(ch >= 32 && ch < 127 ? ch : '?') - 32];
            if (!q.ok) continue;
            if (q.w > 0 && q.h > 0 && ch != ' ')
                image(f.atlas, float(q.ax), float(q.ay), float(q.w), float(q.h), x + q.l * scale, top + q.top * scale, q.w * scale, q.h * scale, alpha, OVER,
                      r / 255.0f, g / 255.0f, b / 255.0f);
            x += (q.l + q.w + q.r) * scale;
        }
        return x;
    }
    void straight(std::vector<uint8_t>& out) const
    {
        out.resize(size_t(w) * size_t(h) * 4);
        for (size_t i = 0; i < size_t(w) * size_t(h); i++) {
            const float a = c[i * 4 + 3];
            for (int k = 0; k < 3; k++) out[i * 4 + k] = uint8_t(std::clamp(a > 0 ? c[i * 4 + k] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[i * 4 + 3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
};

// ---- the model the glue fills ---------------------------------------------------------------------------------------------------------
enum Kind { NONE, MENU, DIALOG };
enum Tone { WHITE, GREY, AMBER };
struct Row { std::string label, value; };
struct Line { std::string s; int tone = WHITE; };
struct Guide { int glyph = jpart::PG_CROSS, glyph2 = -1; std::string s; };   // glyph2: drawn on the same spot (Left + Right: one D-pad, both arms lit)
struct View {
    int kind = NONE;
    std::string title;          // MENU: the header; DIALOG: the message
    std::vector<Row> rows;      // MENU rows; DIALOG: rows[0].label = the button text
    int sel = -1;
    std::vector<Line> lines;
    std::vector<Guide> guide;
    std::string band;           // the one-line band (any kind)
    std::string key() const
    {
        std::string k = std::to_string(kind) + '\x1f' + title + '\x1f' + std::to_string(sel) + '\x1f' + band;
        for (const Row& r : rows) k += '\x1e' + r.label + '\x1d' + r.value;
        for (const Line& l : lines) k += '\x1c' + std::to_string(l.tone) + l.s;
        for (const Guide& g : guide) k += '\x1b' + std::to_string(g.glyph) + ',' + std::to_string(g.glyph2) + g.s;
        return k;
    }
};

// What the overlay draws: `pic` (straight RGBA, pw x ph) at (x, y), then `text` in the overlay's own font on top.
struct TextOp { float x, y; std::string s; float scale; uint32_t rgba; int style; };
struct Frame { int x = 0, y = 0, pw = 0, ph = 0; std::vector<uint8_t> pic; std::vector<TextOp> text; };

inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, float a) { return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24; }

// The painter of one frame: the canvas, its place on screen and the overlay texts.
class Painter {
public:
    using Measure = std::function<float(const std::string&, float)>;   // the overlay font's width (0 when it has none)
    Painter(const jpart::Art* art, Measure ov) : a_(art), ov_(std::move(ov)) {}
    bool font() const { return a_ && a_->font.ok(); }
    float tw(const std::string& s, float scale) const
    {
        if (font()) return a_->font.width(s, scale);
        const float w = ov_ ? ov_(s, scale * kOvScale) : 0.0f;
        return w > 0 ? w : float(s.size()) * 14.0f * scale;
    }
    // s cut to w pixels at this scale ("..." when cut)
    std::string fit(const std::string& s, float w, float scale) const
    {
        if (tw(s, scale) <= w) return s;
        std::string t = s;
        while (!t.empty() && tw(t + "...", scale) > w) t.pop_back();
        while (!t.empty() && t.back() == ' ') t.pop_back();
        return t.empty() ? std::string() : t + "...";
    }
    // body text: the game's font, else the overlay font (style 0) placed at the same baseline
    void text(Canvas& c, int fx, int fy, float x, float base, const std::string& s, float scale, int tone, float alpha, std::vector<TextOp>& ops,
              bool dark = false) const
    {
        uint8_t r = 255, g = 255, b = 255;
        if (dark) r = g = b = 24; else if (tone == GREY) r = g = b = 190; else if (tone == AMBER) { r = 255; g = 204; b = 40; }
        if (font()) { c.text(a_->font, x, base, s, scale, r, g, b, alpha); return; }
        ops.push_back({ fx + x, fy + base - 21.0f * scale * kOvScale, s, scale * kOvScale, rgba(r, g, b, alpha), dark ? 1 : 0 });
    }
    // the 9-slice pause panel (corner size k: 64 = as authored)
    void panel(Canvas& c, float x, float y, float W, float H, float k, float al) const
    {
        if (a_ && a_->panel_ok()) {
            const Image* p = a_->pl;
            const float xs[3] = { x, x + k, x + W - k }, ws[3] = { k, W - 2 * k, k }, ys[3] = { y, y + k, y + H - k }, hs[3] = { k, H - 2 * k, k };
            for (int r = 0; r < 3; r++) for (int q = 0; q < 3; q++) c.image(p[r * 3 + q], xs[q], ys[r], ws[q], hs[r], al);
            return;
        }
        c.fill(x + 2, y + 2, W - 4, H - 4, 0, 0, 0, 0.75f * al);         // fallback: black glass, opaque header band, light rim
        c.fill(x + 2, y + 2, W - 4, std::min(k - 6, H - 4), 0, 0, 0, 0.25f * al);
        c.fill(x, y, W, 2, 200, 200, 200, 0.9f * al); c.fill(x, y + H - 2, W, 2, 200, 200, 200, 0.9f * al);
        c.fill(x, y, 2, H, 200, 200, 200, 0.9f * al); c.fill(x + W - 2, y, 2, H, 200, 200, 200, 0.9f * al);
    }
    void rule(Canvas& c, float x, float y, float W, float al) const
    {
        if (a_ && a_->rule.ok()) c.image(a_->rule, x, y, W, 8, al);
        else c.hgrad(x, y + 3, W, 2, 255, 255, 255, 0.0f, 0.9f * al);
    }
    void row_band(Canvas& c, float x, float y, float W, float H, float al) const
    {
        if (a_ && a_->band.ok()) c.image(a_->band, 0, 0, float(a_->band.w), 50, x, y, W, H, 0.55f * al);
        else c.hgrad(x, y, W, H, 255, 255, 255, 0.0f, 0.22f * al);
    }
    void cursor(Canvas& c, float x, float y, float W, float H, float sweep, float al) const
    {
        const float sw = std::max(1.0f, W * (0.35f + 0.65f * std::clamp(sweep, 0.0f, 1.0f)));
        if (a_ && a_->cursor.ok() && a_->cursorline.ok()) {
            c.image(a_->cursor, 0, 1, float(a_->cursor.w), 41, x + W - sw, y, sw, H, al, GLOW);
            c.image(a_->cursorline, 0, 0, float(a_->cursorline.w), 26, x, y + H - 13, W, 26, al, GLOW);
            return;
        }
        c.hgrad(x + W - sw, y, sw, H, 255, 176, 0, 0.0f, 0.85f * al);
        c.hgrad(x, y + H - 2, W, 2, 255, 220, 80, 0.2f * al, 1.0f * al);
    }
    // a button glyph, size s (fallback: a dark disc with the drawn symbol)
    void glyph(Canvas& c, int g, float x, float y, float s, float al) const
    {
        if (a_ && g >= 0 && g < jpart::PG_N && a_->pad[g].ok()) { c.image(a_->pad[g], x, y, s, s, al); return; }
        const float cx = x + s / 2, cy = y + s / 2, R = s * 0.45f;
        for (int yy = int(y); yy < int(y + s); yy++)
            for (int xx = int(x); xx < int(x + s); xx++) {
                const float dx = xx + 0.5f - cx, dy = yy + 0.5f - cy, d = std::sqrt(dx * dx + dy * dy);
                float cov = std::clamp(R - d + 0.5f, 0.0f, 1.0f);
                if (cov <= 0) continue;
                uint8_t r = 50, gg = 50, b = 56;
                const float u = dx / R, v = dy / R;
                bool mark = false;
                if (g == jpart::PG_CROSS) { mark = (std::fabs(u - v) < 0.2f || std::fabs(u + v) < 0.2f) && std::fabs(u) < 0.55f && std::fabs(v) < 0.55f; if (mark) { r = 130; gg = 150; b = 255; } }
                else if (g == jpart::PG_CIRCLE) { const float q = std::sqrt(u * u + v * v); mark = q > 0.36f && q < 0.6f; if (mark) { r = 255; gg = 80; b = 90; } }
                else { const float t = g == jpart::PG_UP ? -v : g == jpart::PG_DOWN ? v : g == jpart::PG_LEFT ? -u : u, o = (g == jpart::PG_UP || g == jpart::PG_DOWN) ? u : v;
                       mark = t > -0.15f && t < 0.55f && std::fabs(o) < (0.55f - t) * 0.9f; if (mark) { r = 255; gg = 140; b = 0; } }
                c.over(xx, yy, r / 255.0f * cov * al, gg / 255.0f * cov * al, b / 255.0f * cov * al, cov * al);
            }
    }
    // the guide strip; returns its width (measure only when c is null)
    // the guide strip at (x, y), `width` wide (0: as wide as its items); returns its width. The game's sizes: glyph 26 px, text cap 18 px.
    float guide(Canvas* c, int fx, int fy, float x, float y, const std::vector<Guide>& gs, float al, std::vector<TextOp>* ops, float width = 0) const
    {
        const float H = 39, gs_ = 26, sc = 0.75f;
        float w = 36;
        for (const Guide& g : gs) w += gs_ + 6 + tw(g.s, sc) + 24;
        w = std::max(w + 6, width);
        if (!c) return w;
        if (a_ && a_->guide_ok()) {
            const float lw = 32, rw = 32;
            c->image(a_->guide[0], x, y, lw, H, al); c->image(a_->guide[1], x + lw, y, w - lw - rw, H, al); c->image(a_->guide[2], x + w - rw, y, rw, H, al);
        } else { c->fill(x + 10, y + 3, w - 20, H - 6, 196, 196, 196, 0.95f * al); c->fill(x + 10, y + 3, w - 20, 3, 30, 30, 30, al); c->fill(x + 10, y + H - 6, w - 20, 3, 30, 30, 30, al); }
        float px = x + 36;
        for (const Guide& g : gs) {
            glyph(*c, g.glyph, px, y + (H - gs_) / 2, gs_, al);
            if (g.glyph2 >= 0) glyph(*c, g.glyph2, px, y + (H - gs_) / 2, gs_, al);   // the same D-pad with the second arm lit too
            px += gs_;
            px += 6;
            text(*c, fx, fy, px, y + 28, g.s, sc, WHITE, al, *ops, true);
            px += tw(g.s, sc) + 24;
        }
        return w;
    }
    // a menu title: the game's system font in the treatment of its own title plates (chrome white-to-grey, black rim, italic lean);
    // the overlay font when the game's font is not there
    void title(Canvas& c, int fx, int fy, float x, float base, const std::string& s, float scale, float al, std::vector<TextOp>& ops) const
    {
        if (!font()) { ops.push_back({ fx + x, fy + base - 21.0f * 1.1f, s, 1.1f, rgba(255, 255, 255, al), 0 }); return; }
        const jpart::Font& f = a_->font;
        const int pad = 6, tw_ = int(f.width(s, scale)) + 2 * pad + 16, th = int(f.line * scale) + 2 * pad;
        Canvas t; t.make(tw_, th);
        t.text(f, float(pad), pad + f.base * scale, s, scale, 255, 255, 255, 1.0f);
        const float top = pad + (f.base - f.cap) * scale, bot = pad + f.base * scale, lean = 0.2f;
        auto cov = [&t](int x, int y) { return x < 0 || y < 0 || x >= t.w || y >= t.h ? 0.0f : t.c[(size_t(y) * t.w + x) * 4 + 3]; };
        const float ox = x - pad, oy = base - (pad + f.base * scale);
        for (int y = 0; y < th; y++) {
            const float sh = (bot - y) * lean;
            const float g = std::clamp((y - top) / std::max(1.0f, bot - top), 0.0f, 1.0f), v = 1.0f - 0.45f * g * g;   // white -> grey, like the plates
            for (int xx = 0; xx < tw_; xx++) {
                float rim = 0;
                for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) if (dx * dx + dy * dy <= 5) rim = std::max(rim, cov(xx + dx, y + dy));
                const float a = cov(xx, y);
                const int X = int(ox + xx + sh), Y = int(oy + y);
                if (rim > 0) { c.over(X + 1, Y + 2, 0, 0, 0, rim * al * 0.9f); c.over(X, Y, 0, 0, 0, rim * al); }   // the drop and the rim
                if (a > 0) c.over(X, Y, v * a * al, v * a * al, v * a * al, a * al);
            }
        }
    }
    // the dotted rule of the game's notice window (else a faint line)
    void dots(Canvas& c, float x, float y, float W, float al) const
    {
        if (a_ && a_->dots.ok()) c.image(a_->dots, x, y, W, 4, 0.7f * al);
        else c.hgrad(x, y + 1, W, 1, 255, 255, 255, 0.35f * al, 0.0f);
    }
    static constexpr float kOvScale = 0.9f;   // the overlay font is wider: a little smaller at the same nominal size
private:
    const jpart::Art* a_;
    Measure ov_;
};

// ---- the three layouts ------------------------------------------------------------------------------------------------------------------
constexpr int kMenuW = 520, kMenuValuesW = 680, kRowH = 46, kLineH = 30, kMenuX = 700;
constexpr int kLinesGap = 22;     // rows -> status lines (the dotted rule in the gap)
constexpr int kGuideY = 626;      // the game's guide bar: its top at 720p (Main Menu, Character Info)
constexpr float kGlyphGap = 10;   // D-pad glyph <-> value text
inline Frame menu(const Painter& P, const View& v, float open, float sweep)
{
    Frame f;
    const float al = std::clamp(open, 0.0f, 1.0f), lift = (1.0f - al) * 12.0f;
    const int nrows = int(v.rows.size()), nlines = int(v.lines.size());
    const int H = std::max(144, 72 + nrows * kRowH + (nlines ? kLinesGap + nlines * kLineH : 0) + 30);
    bool values = false;
    for (const Row& r : v.rows) values = values || !r.value.empty();
    const int W = values ? kMenuValuesW : kMenuW;
    const float ls = values ? 0.82f : 0.92f;   // label scale: smaller beside a value
    f.x = std::min(kMenuX, 1280 - 40 - W); f.y = int((kGuideY - H) / 2 + lift);
    Canvas c; c.make(W, H);
    P.panel(c, 0, 0, float(W), float(H), 64, al);
    P.title(c, f.x, f.y, 30, 43, v.title, 1.05f, al, f.text);   // the header inside the panel's black top band
    P.rule(c, 26, 52, float(W - 52), al);
    for (int i = 0; i < nrows; i++) {
        const float y = 70.0f + i * kRowH;
        P.row_band(c, 22, y, float(W - 44), 40, al);
        if (i == v.sel) P.cursor(c, 22, y, float(W - 44), 40, sweep, al);
        const bool on = i == v.sel;
        P.text(c, f.x, f.y, 44, y + 29, v.rows[size_t(i)].label, ls, on ? WHITE : GREY, al, f.text);
        const float right = float(W - 44 - 24 - kGlyphGap);   // values end here, the right glyph after it (room kept on every row: values stay aligned)
        const std::string val = P.fit(v.rows[size_t(i)].value, right - 24 - kGlyphGap - (44 + P.tw(v.rows[size_t(i)].label, ls) + 20), 0.86f);
        if (!val.empty()) {
            const float vw = P.tw(val, 0.86f);
            P.text(c, f.x, f.y, right - vw, y + 29, val, 0.86f, WHITE, al, f.text);
            if (on) { P.glyph(c, jpart::PG_LEFT, right - vw - kGlyphGap - 24, y + 8, 24, al); P.glyph(c, jpart::PG_RIGHT, right + kGlyphGap, y + 8, 24, al); }
        }
    }
    if (nlines && nrows) P.dots(c, 30, 70.0f + nrows * kRowH + 6, float(W - 60), al);   // the game's dotted rule between the rows and the status lines
    for (int i = 0; i < nlines; i++)
        P.text(c, f.x, f.y, 30, 70.0f + nrows * kRowH + kLinesGap + i * kLineH + 21, P.fit(v.lines[size_t(i)].s, float(W - 60), 0.72f), 0.72f, v.lines[size_t(i)].tone, al, f.text);
    f.pw = c.w; f.ph = c.h; c.straight(f.pic);
    return f;
}
// The guide bar, where the game's menus have theirs: across the foot of the screen (x 40 to 1240), the items from the left.
inline Frame guide_bar(const Painter& P, const std::vector<Guide>& gs)
{
    Frame f;
    f.x = 40; f.y = kGuideY;
    Canvas c; c.make(1200, 39);
    P.guide(&c, f.x, f.y, 0, 0, gs, 1.0f, &f.text, 1200);
    f.pw = c.w; f.ph = c.h; c.straight(f.pic);
    return f;
}
// The Leave confirm in the game's general dialog. rows[0].label = the button.
inline Frame dialog(const Painter& P, const jpart::Art* a, const View& v, float open)
{
    Frame f;
    const float al = std::clamp(open, 0.0f, 1.0f), lift = (1.0f - al) * 12.0f;
    const int W = 640, H = 256;
    f.x = (1280 - W) / 2; f.y = int((kGuideY - H) / 2 + lift);
    Canvas c; c.make(W, H);
    if (a && a->dialog_ok()) {
        const float s = H / 320.0f, cw = 32 * s;
        c.image(a->win[0], 0, 0, cw, float(H), al); c.image(a->win[2], cw, 0, W - 2 * cw, float(H), al); c.image(a->win[1], W - cw, 0, cw, float(H), al);
        if (a->halftone.ok()) c.image(a->halftone, cw, 10, 200, 156, 0.55f * al, GLOW);
        if (a->swirl.ok()) c.image(a->swirl, W - cw - 230, 40, 220, 132, 0.8f * al);
    } else {
        c.fill(2, 2, W - 4.0f, H - 4.0f, 22, 22, 24, 0.94f * al); c.fill(2, H - 56.0f, W - 4.0f, 54, 0, 0, 0, 0.5f * al);
        c.fill(0, 0, float(W), 2, 150, 150, 150, al); c.fill(0, H - 2.0f, float(W), 2, 150, 150, 150, al); c.fill(0, 0, 2, float(H), 150, 150, 150, al); c.fill(W - 2.0f, 0, 2, float(H), 150, 150, 150, al);
    }
    P.text(c, f.x, f.y, (W - P.tw(v.title, 1.0f)) / 2, 92, v.title, 1.0f, WHITE, al, f.text);
    for (size_t i = 0; i < v.lines.size(); i++) P.text(c, f.x, f.y, (W - P.tw(v.lines[i].s, 0.72f)) / 2, 132 + 28.0f * i, v.lines[i].s, 0.72f, v.lines[i].tone, al, f.text);
    if (!v.rows.empty()) {
        const float bw = 220, bx = (W - bw) / 2, by = 206;
        if (a && a->carsol.ok()) c.image(*(&a->carsol), 0, 0, float(a->carsol.w), float(a->carsol.h), bx, by, bw, 36, al, GLOW);
        else { c.hgrad(bx, by, bw / 2, 36, 255, 210, 0, 0.0f, 0.95f * al); c.hgrad(bx + bw / 2, by, bw / 2, 36, 255, 210, 0, 0.95f * al, 0.0f); }
        const std::string& b = v.rows[0].label;
        P.text(c, f.x, f.y, bx + (bw - P.tw(b, 0.86f)) / 2, by + 27, b, 0.86f, WHITE, al, f.text, true);
    }
    f.pw = c.w; f.ph = c.h; c.straight(f.pic);
    return f;
}
// The one-line band, bottom left (a slim pause panel).
inline Frame band(const Painter& P, const jpart::Art* a, const std::string& s)
{
    Frame f;
    const float sc = 0.8f;
    const bool note = a && a->note[0].ok() && a->note[1].ok() && a->note[2].ok();
    const int H = 64, icon = note && a->busy.ok() ? 44 : 0, W = std::min(1220, int(P.tw(s, sc)) + 56 + icon);
    f.x = 30; f.y = 556;
    Canvas c; c.make(W, H);
    if (note) {   // the game's notice window (its save / load / connecting box), its busy icon at the left
        const float k = H / 228.0f, cw = 64 * k;
        c.image(a->note[0], 0, 0, cw, float(H), 1.0f); c.image(a->note[1], cw, 0, W - 2 * cw, float(H), 1.0f); c.image(a->note[2], W - cw, 0, cw, float(H), 1.0f);
        if (icon) c.image(a->busy, 22, (H - 36) / 2.0f, 36, 36, 1.0f);
    } else P.panel(c, 0, 0, float(W), float(H), 26, 1.0f);
    P.text(c, f.x, f.y, 28.0f + icon, H / 2.0f + 9, s, sc, WHITE, 1.0f, f.text);
    f.pw = c.w; f.ph = c.h; c.straight(f.pic);
    return f;
}

}  // namespace jpskin

#endif
