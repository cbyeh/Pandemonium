#include <libdragon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "ui_pages.h"
#include "globals.h"
#include "video_layout.h"

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

static int lines_in(const char *s, int w);
int ui_text_lines(const char *s, int w) { return lines_in(s, w); }
static int lines_in(const char *s, int w)
{
    int n = (int)strlen(s);
    if (!n) return 1;
    rdpq_paragraph_t *p = rdpq_paragraph_build(&(rdpq_textparms_t){ .width = w, .wrap = WRAP_WORD }, FONT_UNBALANCED, s, &n);
    int k = p->nlines;
    rdpq_paragraph_free(p);
    return k < 1 ? 1 : k;
}

// ---- UiDoc ----

void ui_doc_clear(UiDoc *d) { d->used = 0; d->n = 0; d->pages = 1; d->start[0] = 0; d->start[1] = 0; d->pendingGap = d->pendingBreak = d->pendingKeep = false; }

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
    d->row[d->n].gap = d->pendingGap; d->row[d->n].brk = d->pendingBreak; d->row[d->n].keep = d->pendingKeep;
    d->pendingGap = d->pendingBreak = d->pendingKeep = false;
    d->n++;
}
void ui_doc_gap(UiDoc *d) { if (d->n) d->pendingGap = true; }
void ui_doc_break(UiDoc *d) { if (d->n) d->pendingBreak = true; }
void ui_doc_keep(UiDoc *d) { d->pendingKeep = true; }

static int row_w(const UiDoc *d, int i, int w) { return d->row[i].r >= 0 ? w - UI_RIGHT_W - 4 : w; }

// pages that fit h: a row's lines (word-wrapped to its width) x the pitch, a gap half a line
int ui_doc_layout(UiDoc *d, int w, int h)
{
    int pitch = ui_line_pitch(), used = 0;
    d->pages = 1; d->start[0] = 0;
    for (int i = 0; i < d->n; i++) {
        int rh = lines_in(d->text + d->row[i].l, row_w(d, i, w)) * pitch, gap = d->row[i].gap ? pitch / 2 : 0;
        if (d->row[i].keep && i + 1 < d->n) rh += lines_in(d->text + d->row[i + 1].l, row_w(d, i + 1, w)) * pitch;     // (with the row after it)
        if (i > d->start[d->pages - 1] && (d->row[i].brk || used + gap + rh > h) && d->pages < UI_DOC_PAGES) {
            d->start[d->pages++] = (uint8_t)i;
            used = 0; gap = 0;
        }
        if (d->row[i].keep && i + 1 < d->n) rh -= lines_in(d->text + d->row[i + 1].l, row_w(d, i + 1, w)) * pitch;
        used += gap + rh;
    }
    d->start[d->pages] = (uint8_t)d->n;
    return d->pages;
}

void ui_doc_draw(const UiDoc *d, int page, int x, int y, int w)
{
    if (page < 0 || page >= d->pages) return;
    int pitch = ui_line_pitch();
    for (int i = d->start[page]; i < d->start[page + 1]; i++) {
        if (d->row[i].gap && i > d->start[page]) y += pitch / 2;
        int k = rdpq_text_printf(&(rdpq_textparms_t){ .width = row_w(d, i, w), .wrap = WRAP_WORD, .style_id = d->row[i].style },
                                 FONT_UNBALANCED, x, y, "%s", d->text + d->row[i].l).nlines;
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
    flat();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 140));
    rdpq_fill_rectangle(x0, y0, x1, y1);
    rdpq_set_prim_color(RGBA32(255, 255, 255, 70));
    rdpq_fill_rectangle(x0, y0, x1, y0 + 1);
    rdpq_fill_rectangle(x0, y1 - 1, x1, y1);
    rdpq_fill_rectangle(x0, y0, x0 + 1, y1);
    rdpq_fill_rectangle(x1 - 1, y0, x1, y1);
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
static PanelBox panel_box(const UiPanel *p)
{
    // the title-safe area (never wider than the fixed 7.5% one: the default overscan shows more, a larger one less)
    int mx = ui_safe_margin_x(), my = ui_safe_margin_y();
    if (mx < TITLE_SAFE_MARGIN_X) mx = TITLE_SAFE_MARGIN_X;
    if (my < TITLE_SAFE_MARGIN_Y) my = TITLE_SAFE_MARGIN_Y;
    PanelBox b = { mx, my, SCREEN_WIDTH - mx, SCREEN_HEIGHT - my };
    int pitch = ui_line_pitch();
    b.tx = b.x0 + 6; b.tw = b.x1 - b.x0 - 12;
    b.corners = b.y0 + 14;
    b.title = p->confirm ? b.corners + 6 : b.corners + pitch + 2;     // (a confirm has no corners: its title on their row)
    b.body = b.title + lines_in(p->title, b.tw) * pitch + 4;     // (a long title wraps: the page starts under it)
    b.foot = b.y1 - 6;
    b.bodyEnd = p->confirm ? b.foot - 22 : b.foot - pitch;      // the last baseline a body line may take
    return b;
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
    flat();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 120));      // the screen under it dimmed
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    rdpq_set_prim_color(RGBA32(180, 160, 120, 255));    // the pause menu's frame
    rdpq_fill_rectangle(b.x0, b.y0, b.x1, b.y1);
    rdpq_set_prim_color(RGBA32(24, 18, 14, 250));
    rdpq_fill_rectangle(b.x0 + 2, b.y0 + 2, b.x1 - 2, b.y1 - 2);
    if (!p->confirm) ui_pager_draw(&p->pg, b.tx, b.corners, b.tw, NULL);
    else if (p->pg.pages > 1) {     // (a confirm's pages: Up/Down)
        char n[24]; snprintf(n, sizeof n, "Up/Down %d/%d", p->pg.page + 1, p->pg.pages);
        ui_hint(b.tx + b.tw, b.foot - 22, n);
    }
    rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = b.tw, .wrap = WRAP_WORD, .style_id = p->titleStyle }, FONT_UNBALANCED, b.tx, b.title, "%s", p->title);
    if (p->doc) ui_doc_draw(p->doc, p->pg.page, b.tx, b.body, b.tw);
    if (p->confirm) {
        const int bw = 90, gap = 16, bx = b.tx + (b.tw - 2 * bw - gap) / 2, by = b.foot - 15;
        for (int i = 0; i < 2; i++) {
            int x = bx + i * (bw + gap);
            bool on = p->button == i;
            flat();
            rdpq_set_prim_color(on ? RGBA32(255, 215, 90, 255) : RGBA32(110, 100, 90, 255));
            rdpq_fill_rectangle(x - 2, by - 2, x + bw + 2, by + 16);
            rdpq_set_prim_color(on ? RGBA32(60, 30, 20, 240) : RGBA32(30, 22, 18, 230));
            rdpq_fill_rectangle(x, by, x + bw, by + 14);
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = bw, .style_id = on ? FONT_STYLE_GOLD : FONT_STYLE_WHITE },
                             FONT_UNBALANCED, x, by + 11, "%s", i ? "Back (B)" : "Enter (A)");
        }
    } else ui_hint(b.tx + b.tw, b.foot, "R or B: close");
}
