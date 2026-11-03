#ifndef UI_PAGES_H
#define UI_PAGES_H

#include <stdbool.h>
#include <stdint.h>

// One way to show more text than fits (the pause menu's Stats and Build pages, the Tower's screens, the
// R Details panel; a Codex can reuse all of it as is). The style is the Stats pages' (main 83f1c7b, b5ecdae):
// the page's buttons sit in the title row's corners, page 0 "Back" | "Next >", a middle page
// "< Prev" | "Next >", the last "< Prev" (a lone page: "Back"); Left/Right pick a corner, A takes it,
// B is Back from any page. Every press works the same on the D-pad and the stick (the callers pass edges).
//
// UiDoc: rows of text (a left part that word-wraps, an optional right part on its first line, a font
// style), split into pages that fit a box by ui_doc_layout (measured with the font), one page drawn by
// ui_doc_draw. Fill it each time it's shown (cheap: the menus do it every frame).
//     ui_doc_clear(&d); ui_doc_row(&d, FONT_STYLE_GOLD, "Lv 2/3", "Rally"); ui_doc_gap(&d); ui_doc_text(&d, FONT_STYLE_WHITE, "...");
//     int pages = ui_doc_layout(&d, w, h); ui_doc_draw(&d, page, x, y, w);
// UiPager: the corners (ui_pager_reset when the doc opens, ui_pager_input with the edges, ui_pager_draw).
// UiPanel: a framed box inside the title-safe area holding a title, a UiDoc and a UiPager: the Details
// panel (ui_details(): one shared instance) and the Tower map's confirm screen (ui_panel_open with confirm).
//     UiPanel *p = ui_details(); UiDoc *d = ui_panel_doc(p); ui_doc_clear(d); ...rows...; ui_panel_open(p, "Rally", FONT_STYLE_BLUE, false);
//     each frame: if (p->open) { ui_panel_input(p, l, r, a, b, rPress); ui_panel_draw(p); }

#define UI_DOC_ROWS  64          // (the Codex's longest page: a sentence a row)
#define UI_DOC_TEXT  3072
#define UI_DOC_PAGES 10
#define UI_RIGHT_W   64          // the right part's column

typedef struct {
    char text[UI_DOC_TEXT]; int used;
    struct { int16_t l, r; uint8_t style, gap, brk, keep, cont; } row[UI_DOC_ROWS];     // cont: split off the row before it by a layout
    int n;
    uint8_t start[UI_DOC_PAGES + 1];    // the first row of each page (start[pages] = n)
    int pages;
    bool pendingGap, pendingBreak, pendingKeep;
} UiDoc;

void ui_doc_clear(UiDoc *d);
void ui_doc_row(UiDoc *d, int style, const char *right, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#define ui_doc_text(d, style, ...) ui_doc_row(d, style, NULL, __VA_ARGS__)
void ui_doc_gap(UiDoc *d);              // half a line before the next row (dropped at a page's top)
void ui_doc_break(UiDoc *d);            // the next row starts a page
void ui_doc_keep(UiDoc *d);             // the next row stays on the page of the one after it (a heading)
int  ui_doc_layout(UiDoc *d, int w, int h);     // returns the pages (1 at least)
void ui_doc_draw(const UiDoc *d, int page, int x, int y, int w);     // y: the first line's baseline
int  ui_line_pitch(void);               // the font's line pitch (FONT_UNBALANCED)
int  ui_text_width(const char *s);      // one line's width in pixels
int  ui_text_lines(const char *s, int w);   // its lines, word-wrapped to w

typedef struct { int8_t page, pages, sel; } UiPager;   // sel: 0 the left corner, 1 the right
enum { UI_PAGER_NONE, UI_PAGER_TURNED, UI_PAGER_BACK };
void ui_pager_reset(UiPager *p, int pages);     // page 0, the cursor on Next (Back on a lone page)
int  ui_pager_input(UiPager *p, bool left, bool right, bool a, bool b);
void ui_pager_draw(const UiPager *p, int x, int y, int w, const char *title);     // the corners, the title and "2/3" between; y: the row's baseline

// the menus' selection box (black 140 under a 1 px white 70 frame)
void ui_select_box(int x0, int y0, int x1, int y1);
// a small grey hint ("R Details") ending at x (right-aligned), y its baseline
void ui_hint(int xRight, int y, const char *s);

typedef struct {
    bool open, confirm;                 // confirm: Enter and Back buttons under the page (the map's)
    uint8_t owner;                      // who opened it (UI_OWNER_): each screen draws and drives only its own
    int8_t button;                      // confirm: 0 Enter, 1 Back
    char title[64]; int titleStyle;
    UiDoc *doc;                         // its page, on the heap while it's open (ui_panel_doc): none during a fight
    UiPager pg;
} UiPanel;
enum { UI_OWNER_TOWER, UI_OWNER_MENU };
UiPanel *ui_details(void);              // the shared Details panel (and the map's confirm screen)
UiDoc *ui_panel_doc(UiPanel *p);        // its doc to fill (allocated on first use)
void ui_panel_close(UiPanel *p);        // closed, its doc freed
void ui_panel_open(UiPanel *p, const char *title, int titleStyle, bool confirm);
// the presses (edges): Up/Down (a confirm's pages), Left/Right, A, B, R. Returns UI_PANEL_ENTER when a confirm
// panel's Enter is taken; B, R (not on a confirm) or Back close it
enum { UI_PANEL_STAY, UI_PANEL_CLOSED, UI_PANEL_ENTER };
int  ui_panel_input(UiPanel *p, bool up, bool down, bool left, bool right, bool a, bool b, bool r);
void ui_panel_draw(const UiPanel *p);

#endif
