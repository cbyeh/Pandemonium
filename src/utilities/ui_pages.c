#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <rdpq_paragraph.h>

#include "ui_pages.h"
#include "globals.h"
#include "video_layout.h"

// ---- Tracking what's laid out, measuring a screen, fitting its box (ui_pages.h) ----

static bool s_measuring, s_foot;
static UiRect s_trk[2]; static bool s_trkAny[2];
static UiRect s_lastBox;
void ui_track(int x0, int y0, int x1, int y1)
{
    UiRect *r = &s_trk[s_foot];
    if (!s_trkAny[s_foot]) { *r = (UiRect){ x0, y0, x1, y1 }; s_trkAny[s_foot] = true; return; }
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}
void ui_track_foot(bool foot) { s_foot = foot; }
static void track_reset(void) { s_trkAny[0] = s_trkAny[1] = false; s_foot = false; }
void ui_measure_begin(void) { s_measuring = true; track_reset(); rdpq_set_scissor(0, 0, 0, 0); }
void ui_measure_end(void) { s_measuring = false; s_foot = false; rdpq_set_scissor(0, 0, display_get_width(), display_get_height()); }
bool ui_measuring(void) { return s_measuring; }
UiRect ui_tracked(bool foot) { return s_trkAny[foot] ? s_trk[foot] : (UiRect){ 0, 0, 0, 0 }; }
UiRect ui_last_box(void) { return s_lastBox; }

// From a measured pass laid out in `d` (the safe rect): the box (*box) at a fixed width (UI_NARROW, or UI_WIDE: the safe
// rect's), as tall as its content and UI_EVEN round it, centred in d; and the rect to lay it out in again (moved so the
// content lands UI_EVEN inside the box, its footer as far over the box's bottom)
UiRect ui_fit(UiRect d, int width, UiRect *box)
{
    UiRect b = s_trk[0], r = d;
    if (!s_trkAny[0]) { *box = (UiRect){ d.x0 - UI_PAD, d.y0 - UI_PAD, d.x1 + UI_PAD, d.y1 + UI_PAD }; return d; }
    int cx = (d.x0 + d.x1) / 2, W = width == UI_WIDE ? d.x1 - d.x0 + 2 * UI_PAD : width;     // (wide: a Details panel's, the paged screens')
    box->x0 = cx - W / 2; box->x1 = box->x0 + W;
    r.x0 = box->x0 + UI_EVEN - 4; r.x1 = box->x1 - UI_EVEN + 4;     // (a list's cursor 4 outside its rows, centred text 8 inside the rect)
    int top = b.y0 - d.y0, bodyH = b.y1 - b.y0;
    int footUp = s_trkAny[1] ? d.y1 - s_trk[1].y0 : 0, footDown = s_trkAny[1] ? d.y1 - s_trk[1].y1 : 0;
    int H = UI_EVEN + bodyH + (s_trkAny[1] ? 6 + footUp - footDown : 0) + UI_EVEN;
    int maxH = d.y1 - d.y0 + 2 * UI_PAD;
    if (H > maxH) H = maxH;
    box->y0 = (d.y0 + d.y1) / 2 - H / 2; box->y1 = box->y0 + H;
    r.y0 = box->y0 + UI_EVEN - top;
    r.y1 = s_trkAny[1] ? box->y1 - UI_EVEN + footDown : r.y0 + (d.y1 - d.y0);
    return r;
}

rdpq_textmetrics_t ui_textf(const rdpq_textparms_t *parms, uint8_t font, float x, float y, const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n <= 0) return (rdpq_textmetrics_t){ 0 };
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    int len = n;
    rdpq_paragraph_t *p = rdpq_paragraph_build(parms, font, buf, &len);     // (on the heap: a word cut with an ellipsis can't overrun it)
    ui_track((int)(x + p->bbox.x0), (int)(y + p->bbox.y0), (int)(x + p->bbox.x1 + 0.99f), (int)(y + p->bbox.y1 + 0.99f));
    if (!s_measuring) rdpq_paragraph_render(p, x, y);
    rdpq_textmetrics_t m = { .advance_x = p->advance_x, .advance_y = p->advance_y, .nlines = p->nlines, .utf8_text_advance = len };
    rdpq_paragraph_free(p);
    return m;
}
#define rdpq_text_printf ui_textf

// ---- Text measures ----

static int s_pitch;
int ui_line_pitch(void)
{
    if (!s_pitch) {     // the font's line pitch: two lines' height less one's
        int n1 = 1, n2 = 3;
        rdpq_paragraph_t *a = rdpq_paragraph_build(&(rdpq_textparms_t){ 0 }, FONT_UNBALANCED, "A", &n1);
        rdpq_paragraph_t *b = rdpq_paragraph_build(&(rdpq_textparms_t){ 0 }, FONT_UNBALANCED, "A\nA", &n2);
        s_pitch = (int)(b->bbox.y1 - a->bbox.y1 + 0.5f);
        rdpq_paragraph_free(a); rdpq_paragraph_free(b);
        if (s_pitch < 10 || s_pitch > 24) s_pitch = 16;
    }
    return s_pitch;
}

int ui_text_width(const char *s)
{
    int n = (int)strlen(s);
    if (!n) return 0;
    rdpq_paragraph_t *p = rdpq_paragraph_build(&(rdpq_textparms_t){ 0 }, FONT_UNBALANCED, s, &n);
    int w = (int)(p->bbox.x1 - p->bbox.x0 + 0.5f);
    rdpq_paragraph_free(p);
    return w;
}

static int lines_cs(const char *s, int w, int cs)
{
    int n = (int)strlen(s);
    if (!n) return 1;
    int n0 = n;
    rdpq_paragraph_t *p = rdpq_paragraph_build(&(rdpq_textparms_t){ .width = w, .wrap = WRAP_WORD, .char_spacing = cs }, FONT_UNBALANCED, s, &n);
    rdpq_paragraph_t *q = rdpq_paragraph_build(&(rdpq_textparms_t){ .char_spacing = cs }, FONT_UNBALANCED, s, &n0);
    // a word wider than w: libdragon cuts it with an ellipsis (and its glyphs can overrun rdpq_text_printf's stack
    // buffer: a crash). Counted as far too many lines, so every fit check fails it
    bool cut = p->nchars != q->nchars;
    for (int i = 0; !cut && i < p->nchars; i++) cut = p->chars[i].glyph != q->chars[i].glyph;     // (a wrap only moves glyphs)
    int k = cut ? UI_TOO_WIDE : p->nlines;
    rdpq_paragraph_free(p); rdpq_paragraph_free(q);
    return k < 1 ? 1 : k;
}
static int lines_in(const char *s, int w) { return lines_cs(s, w, 0); }
int ui_text_lines(const char *s, int w) { return lines_in(s, w); }
int ui_text_lines_cs(const char *s, int w, int cs) { return lines_cs(s, w, cs); }

// ---- UiDoc ----

void ui_doc_clear(UiDoc *d) { d->used = 0; d->n = 0; d->pages = 1; d->tallest = 0; d->start[0] = 0; d->start[1] = 0; d->pendingGap = d->pendingBreak = d->pendingKeep = false; d->pendingIcon = 0; }
int (*ui_icon_hook)(uint8_t icon, int x, int y, bool draw);
static int icon_w(uint8_t icon) { return icon && ui_icon_hook ? ui_icon_hook(icon, 0, 0, false) : 0; }

static int doc_put(UiDoc *d, const char *s)
{
    int len = (int)strlen(s);
    if (d->used + len + 1 > UI_DOC_TEXT) len = UI_DOC_TEXT - d->used - 1;
    if (len < 0) return -1;
    int at = d->used;
    memcpy(d->text + at, s, len); d->text[at + len] = 0;
    d->used += len + 1;
    return at;
}

void ui_doc_row(UiDoc *d, int style, const char *right, const char *fmt, ...)
{
    if (d->n >= UI_DOC_ROWS) return;
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    int l = doc_put(d, buf), r = right && right[0] ? doc_put(d, right) : -1;
    if (l < 0) return;
    d->row[d->n].l = (int16_t)l; d->row[d->n].r = (int16_t)r; d->row[d->n].style = (uint8_t)style;
    d->row[d->n].gap = d->pendingGap; d->row[d->n].brk = d->pendingBreak; d->row[d->n].keep = d->pendingKeep; d->row[d->n].cont = 0;
    d->row[d->n].icon = d->pendingIcon;
    d->pendingGap = d->pendingBreak = d->pendingKeep = false; d->pendingIcon = 0;
    d->n++;
}
void ui_doc_gap(UiDoc *d) { if (d->n) d->pendingGap = true; }
void ui_doc_break(UiDoc *d) { if (d->n) d->pendingBreak = true; }
void ui_doc_keep(UiDoc *d) { d->pendingKeep = true; }
void ui_doc_icon(UiDoc *d, uint8_t icon) { d->pendingIcon = icon; }

static int row_w(const UiDoc *d, int i, int w) { return (d->row[i].r >= 0 ? w - UI_RIGHT_W - 4 : w) - icon_w(d->row[i].icon); }     // (a split row keeps its icon's indent)

// A row of sentences that doesn't fit the room left: split after the last sentence that does (the rest a row of
// its own after it, which turns the page), so a long paragraph fills the page instead of leaving it half empty
static void split_to_fit(UiDoc *d, int i, int w, int room)
{
    if (d->n >= UI_DOC_ROWS || d->row[i].r >= 0 || d->row[i].keep) return;
    char *t = d->text + d->row[i].l;
    int pitch = ui_line_pitch(), cut = -1;
    for (char *c = strstr(t, ". "); c; c = strstr(c + 1, ". ")) {
        char keep = c[1]; c[1] = 0;
        bool fits = lines_in(t, row_w(d, i, w)) * pitch <= room;
        c[1] = keep;
        if (!fits) break;
        cut = (int)(c + 1 - t);
    }
    if (cut < 0) return;
    t[cut] = 0;     // (the space after the period: the rest starts past it)
    memmove(&d->row[i + 2], &d->row[i + 1], (d->n - i - 1) * sizeof d->row[0]);
    d->row[i + 1] = d->row[i];
    d->row[i + 1].l = (int16_t)(d->row[i].l + cut + 1);
    d->row[i + 1].gap = d->row[i + 1].brk = d->row[i + 1].keep = 0; d->row[i + 1].cont = 1;
    d->n++;
}
static void rejoin(UiDoc *d)     // the rows a layout split, whole again (a layout at another size splits them its own way)
{
    for (int i = d->n - 1; i > 0; i--) if (d->row[i].cont) {
        d->text[d->row[i].l - 1] = ' ';
        memmove(&d->row[i], &d->row[i + 1], (d->n - i - 1) * sizeof d->row[0]);
        d->n--;
    }
}

// pages that fit h: a row's lines (word-wrapped to its width) x the pitch, a gap half a line
int ui_doc_layout(UiDoc *d, int w, int h)
{
    int pitch = ui_line_pitch(), used = 0, tall = 0;
    rejoin(d);
    d->pages = 1; d->start[0] = 0;
    for (int i = 0; i < d->n; i++) {
        int rh = lines_in(d->text + d->row[i].l, row_w(d, i, w)) * pitch, gap = d->row[i].gap ? pitch / 2 : 0;
        if (!d->row[i].brk && used + gap + rh > h) {
            int room = i > d->start[d->pages - 1] ? h - used - gap : h;
            if (room >= pitch) { split_to_fit(d, i, w, room); rh = lines_in(d->text + d->row[i].l, row_w(d, i, w)) * pitch; }
        }
        if (d->row[i].keep && i + 1 < d->n) rh += lines_in(d->text + d->row[i + 1].l, row_w(d, i + 1, w)) * pitch;     // (with the row after it)
        if (i > d->start[d->pages - 1] && (d->row[i].brk || used + gap + rh > h) && d->pages < UI_DOC_PAGES) {
            d->start[d->pages++] = (uint8_t)i;
            used = 0; gap = 0;
        }
        if (d->row[i].keep && i + 1 < d->n) rh -= lines_in(d->text + d->row[i + 1].l, row_w(d, i + 1, w)) * pitch;
        used += gap + rh;
        if (used > tall) tall = used;
    }
    d->start[d->pages] = (uint8_t)d->n;
    d->tallest = (int16_t)tall;
    return d->pages;
}

void ui_doc_draw(const UiDoc *d, int page, int x, int y, int w)
{
    if (page < 0 || page >= d->pages) return;
    int pitch = ui_line_pitch();
    for (int i = d->start[page]; i < d->start[page + 1]; i++) {
        if (d->row[i].gap && i > d->start[page]) y += pitch / 2;
        int in = icon_w(d->row[i].icon);
        if (in && !d->row[i].cont) ui_icon_hook(d->row[i].icon, x, y, true);
        int k = rdpq_text_printf(&(rdpq_textparms_t){ .width = row_w(d, i, w), .wrap = WRAP_WORD, .style_id = d->row[i].style },
                                 FONT_UNBALANCED, x + in, y, "%s", d->text + d->row[i].l).nlines;
        if (d->row[i].r >= 0)
            rdpq_text_printf(&(rdpq_textparms_t){ .width = UI_RIGHT_W, .align = ALIGN_RIGHT, .style_id = d->row[i].style },
                             FONT_UNBALANCED, x + w - UI_RIGHT_W, y, "%s", d->text + d->row[i].r);
        y += (k < 1 ? 1 : k) * pitch;
    }
}

// ---- UiPager: the title row's corners ----

static bool has_right(const UiPager *p) { return p->page < p->pages - 1; }
void ui_pager_reset(UiPager *p, int pages) { p->page = 0; p->pages = (int8_t)(pages < 1 ? 1 : pages); p->sel = p->pages > 1; }

int ui_pager_input(UiPager *p, bool left, bool right, bool a, bool b)
{
    if (b) return UI_PAGER_BACK;
    if (left) p->sel = 0;
    else if (right && has_right(p)) p->sel = 1;
    if (!a) return UI_PAGER_NONE;
    if (p->sel == 0 && p->page == 0) return UI_PAGER_BACK;     // page 0's left corner: Back
    p->page += p->sel ? 1 : -1;
    // on along the way it went: Next while there's a next (else Prev), Prev while there's a previous (else Next)
    p->sel = p->sel ? has_right(p) : p->page == 0;
    return UI_PAGER_TURNED;
}

void ui_pager_draw(const UiPager *p, int x, int y, int w, const char *title)
{
    const int cw = 64;
    for (int i = 0; i < 2; i++) {
        if (i && !has_right(p)) break;
        int cx = i ? x + w - cw : x;
        if (i == p->sel) ui_select_box(cx - 4, y - 12, cx + cw + 4, y + 4);
        const char *label = i ? "Next >" : p->page ? "< Prev" : "Back";
        rdpq_text_printf(&(rdpq_textparms_t){ .align = i ? ALIGN_RIGHT : ALIGN_LEFT, .width = cw }, FONT_UNBALANCED, cx, y, "%s", label);
    }
    // between them the title (if any) and where it is: "BUILD 2/3"
    char n[48];
    if (p->pages > 1) snprintf(n, sizeof n, "%s%s%d/%d", title ? title : "", title ? "  " : "", p->page + 1, p->pages);
    else snprintf(n, sizeof n, "%s", title ? title : "");
    if (n[0]) rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = title ? FONT_STYLE_WHITE : FONT_STYLE_GREY }, FONT_UNBALANCED, x, y, "%s", n);
}

static void flat(void)
{
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
}

void ui_select_box(int x0, int y0, int x1, int y1)
{
    ui_track(x0, y0, x1, y1);
    if (s_measuring) return;
    flat();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 140));
    rdpq_fill_rectangle(x0, y0, x1, y1);
    rdpq_set_prim_color(RGBA32(255, 255, 255, 70));
    rdpq_fill_rectangle(x0, y0, x1, y0 + 1);
    rdpq_fill_rectangle(x0, y1 - 1, x1, y1);
    rdpq_fill_rectangle(x0, y0, x0 + 1, y1);
    rdpq_fill_rectangle(x1 - 1, y0, x1, y1);
}

UiRect ui_safe_rect(void)
{
    int mx = ui_safe_margin_x(), my = ui_safe_margin_y();
    if (mx < TITLE_SAFE_MARGIN_X) mx = TITLE_SAFE_MARGIN_X;
    if (my < TITLE_SAFE_MARGIN_Y) my = TITLE_SAFE_MARGIN_Y;
    return (UiRect){ mx, my, SCREEN_WIDTH - mx, SCREEN_HEIGHT - my };
}

// The box (the user's pick, 2026-11-03): rounded corners with a soft rim (main's dialog art's anti-aliased edge) in
// translucent black, UI_SHADE: a 9-slice of one 16x16 I4 corner (assets/ui/box_corner.i4.png, tools/tower_icons.py: its
// coverage the alpha, its last column the top edge's profile, its last row the left's), mirrored to the four corners, the
// edges its last row/column stretched, the middle a flat fill. A small texture renders the same on Delta's, Mupen's and
// Ares' renderers (the 70x135 dialog sprite never showed on Delta's)
static sprite_t *s_corner;
#define CORNER 16
void ui_box_exact(int x0, int y0, int x1, int y1)
{
    if (s_measuring) return;
    track_reset();      // (a box starts its screen: what's tracked from here is what's in it)
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_WIDTH) x1 = SCREEN_WIDTH;
    if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT;
    s_lastBox = (UiRect){ x0, y0, x1, y1 };
    if (!s_corner) s_corner = sprite_load("rom:/ui/box_corner.i4.sprite");
    if (x1 - x0 < 2 * CORNER || y1 - y0 < 2 * CORNER || !s_corner) {     // (too small for the corners: a plain fill)
        flat();
        rdpq_set_prim_color(RGBA32(0, 0, 0, UI_SHADE));
        rdpq_fill_rectangle(x0, y0, x1, y1);
        return;
    }
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, PRIM), (TEX0, 0, PRIM, 0)));     // black, its alpha the texel's x the shade
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(0, 0, 0, UI_SHADE));
    const int ex = x1 - x0 - 2 * CORNER, ey = y1 - y0 - 2 * CORNER;     // the edges' lengths
    for (int k = 0; k < 4; k++) {
        bool fx = k & 1, fy = k & 2;
        int cx = fx ? x1 - CORNER : x0, cy = fy ? y1 - CORNER : y0;
        rdpq_sprite_blit(s_corner, cx, cy, &(rdpq_blitparms_t){ .flip_x = fx, .flip_y = fy });
        // the top/bottom edge (the corner's last column, stretched) and the left/right (its last row), each once
        if (!fx && ex > 0) rdpq_sprite_blit(s_corner, x0 + CORNER, cy, &(rdpq_blitparms_t){ .s0 = CORNER - 1, .width = 1, .flip_y = fy, .scale_x = (float)ex });
        if (!fy && ey > 0) rdpq_sprite_blit(s_corner, cx, y0 + CORNER, &(rdpq_blitparms_t){ .t0 = CORNER - 1, .height = 1, .flip_x = fx, .scale_y = (float)ey });
    }
    flat();
    rdpq_set_prim_color(RGBA32(0, 0, 0, UI_SHADE));
    if (ex > 0 && ey > 0) rdpq_fill_rectangle(x0 + CORNER, y0 + CORNER, x1 - CORNER, y1 - CORNER);
}
// a box UI_PAD past the rect asked for (its layout rect), clamped to the screen
void ui_box(int x0, int y0, int x1, int y1) { ui_box_exact(x0 - UI_PAD, y0 - UI_PAD, x1 + UI_PAD, y1 + UI_PAD); }

void ui_frame(int x0, int y0, int x1, int y1, int t)
{
    rdpq_fill_rectangle(x0, y0, x1, y0 + t);
    rdpq_fill_rectangle(x0, y1 - t, x1, y1);
    rdpq_fill_rectangle(x0, y0 + t, x0 + t, y1 - t);
    rdpq_fill_rectangle(x1 - t, y0 + t, x1, y1 - t);
}

void ui_button(int x, int y, int w, int h, bool on, bool ok, const char *label)
{
    ui_track(x - 2, y - 2, x + w + 2, y + h + 2);
    flat();
    rdpq_set_prim_color(RGBA32(0, 0, 0, UI_SHADE));
    rdpq_fill_rectangle(x, y, x + w, y + h);
    rdpq_set_prim_color(on ? RGBA32(255, 215, 90, 255) : RGBA32(110, 100, 90, 255));
    ui_frame(x - 2, y - 2, x + w + 2, y + h + 2, 2);
    rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = !ok ? FONT_STYLE_GREY : on ? FONT_STYLE_GOLD : FONT_STYLE_WHITE },
                     FONT_UNBALANCED, x, y + h - 3, "%s", label);
}

void ui_hint(int xRight, int y, const char *s)
{
    rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = 120, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, xRight - 120, y, "%s", s);
}

// ---- UiPanel: framed, inside the title-safe area at any overscan setting ----

static UiPanel s_details;
UiPanel *ui_details(void) { return &s_details; }
UiDoc *ui_panel_doc(UiPanel *p)
{
    if (!p->doc) { p->doc = malloc(sizeof(UiDoc)); ui_doc_clear(p->doc); }
    return p->doc;
}
void ui_panel_close(UiPanel *p) { p->open = false; free(p->doc); p->doc = NULL; }

typedef struct { int x0, y0, x1, y1, tx, tw, corners, title, body, bodyEnd, foot; } PanelBox;
static PanelBox panel_box_in(const UiPanel *p, UiRect r)
{
    PanelBox b = { r.x0, r.y0, r.x1, r.y1 };
    int pitch = ui_line_pitch();
    b.tx = b.x0 + 6; b.tw = b.x1 - b.x0 - 12;
    b.corners = b.y0 + UI_TITLE_DY;
    b.title = p->confirm ? b.corners + 6 : b.corners + pitch + 2;     // (a confirm has no corners: its title on their row)
    b.body = b.title + lines_in(p->title, b.tw) * pitch + 4;     // (a long title wraps: the page starts under it)
    b.foot = b.y1 - UI_FOOT_DY;
    b.bodyEnd = p->confirm ? b.foot - 22 : b.foot - pitch;      // the last baseline a body line may take
    return b;
}
// the menus' box: the title-safe area's width (never wider than the fixed 7.5% one), as tall as its tallest page (laid out
// for the whole area: its pages don't change), centred
static PanelBox panel_box(const UiPanel *p)
{
    UiRect r = ui_safe_rect();
    PanelBox b = panel_box_in(p, r);
    if (!p->doc || p->doc->tallest <= 0) return b;
    int pitch = ui_line_pitch(), lastBase = b.body + p->doc->tallest - pitch;
    if (p->pg.pages > 1 && p->confirm) lastBase += pitch;     // (a confirm's "Up/Down 1/2" line over its buttons)
    int h = lastBase - b.bodyEnd + (r.y1 - r.y0);       // as far under the last line as the area's bottom is under bodyEnd
    if (h >= r.y1 - r.y0) return b;
    r.y0 += (r.y1 - r.y0 - h) / 2; r.y1 = r.y0 + h;
    return panel_box_in(p, r);
}

void ui_panel_open(UiPanel *p, const char *title, int titleStyle, bool confirm)
{
    snprintf(p->title, sizeof p->title, "%s", title);
    p->titleStyle = titleStyle;
    p->confirm = confirm;
    p->button = 0;
    PanelBox b = panel_box(p);
    int h = b.bodyEnd - b.body + ui_line_pitch(), pages = ui_doc_layout(ui_panel_doc(p), b.tw, h);
    if (confirm && pages > 1) pages = ui_doc_layout(p->doc, b.tw, h - ui_line_pitch());     // (room for its "Up/Down 1/2")
    ui_pager_reset(&p->pg, pages);
    p->open = true;
}

int ui_panel_input(UiPanel *p, bool up, bool down, bool left, bool right, bool a, bool b, bool r)
{
    if (!p->open) return UI_PANEL_STAY;
    if (p->confirm) {       // Enter | Back under the page; a long one turns with Up/Down ("Up/Down 1/2" over it)
        if (b) { ui_panel_close(p); return UI_PANEL_CLOSED; }
        if (down && p->pg.page < p->pg.pages - 1) p->pg.page++;
        if (up && p->pg.page > 0) p->pg.page--;
        if (left) p->button = 0;
        if (right) p->button = 1;
        if (!a) return UI_PANEL_STAY;
        ui_panel_close(p);
        return p->button ? UI_PANEL_CLOSED : UI_PANEL_ENTER;
    }
    if (r || ui_pager_input(&p->pg, left, right, a, b) == UI_PAGER_BACK) { ui_panel_close(p); return UI_PANEL_CLOSED; }
    return UI_PANEL_STAY;
}

void ui_panel_draw(const UiPanel *p)
{
    if (!p->open) return;
    PanelBox b = panel_box(p);
    ui_box(b.x0, b.y0, b.x1, b.y1);     // (drawn alone: the screen under it isn't drawn while it's open)
    if (!p->confirm) ui_pager_draw(&p->pg, b.tx, b.corners, b.tw, NULL);
    else if (p->pg.pages > 1) {     // (a confirm's pages: Up/Down)
        char n[24]; snprintf(n, sizeof n, "Up/Down %d/%d", p->pg.page + 1, p->pg.pages);
        ui_hint(b.tx + b.tw, b.foot - 22, n);
    }
    rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = b.tw, .wrap = WRAP_WORD, .style_id = p->titleStyle }, FONT_UNBALANCED, b.tx, b.title, "%s", p->title);
    if (p->doc) ui_doc_draw(p->doc, p->pg.page, b.tx, b.body, b.tw);
    if (p->confirm) {
        const int bw = 90, gap = 16, bx = b.tx + (b.tw - 2 * bw - gap) / 2, by = b.foot - 15;
        for (int i = 0; i < 2; i++) ui_button(bx + i * (bw + gap), by, bw, 14, p->button == i, true, i ? "Back" : "Enter");
    } else ui_hint(b.tx + b.tw, b.foot, "Close (R)");
}
