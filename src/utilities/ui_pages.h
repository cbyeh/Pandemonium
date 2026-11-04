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
    struct { int16_t l, r; uint8_t style, gap, brk, keep, cont, icon; } row[UI_DOC_ROWS];     // cont: split off the row before it by a layout
    int n;
    uint8_t start[UI_DOC_PAGES + 1];    // the first row of each page (start[pages] = n)
    int pages;
    bool pendingGap, pendingBreak, pendingKeep;
    uint8_t pendingIcon;
    int16_t tallest;                    // the tallest page's height, by the last layout (a panel fits it)
} UiDoc;

void ui_doc_clear(UiDoc *d);
void ui_doc_row(UiDoc *d, int style, const char *right, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#define ui_doc_text(d, style, ...) ui_doc_row(d, style, NULL, __VA_ARGS__)
void ui_doc_gap(UiDoc *d);              // half a line before the next row (dropped at a page's top)
void ui_doc_break(UiDoc *d);            // the next row starts a page
void ui_doc_keep(UiDoc *d);             // the next row stays on the page of the one after it (a heading)
void ui_doc_icon(UiDoc *d, uint8_t icon);   // the next row has this icon left of its text (drawn by ui_icon_hook; its text indented)
// draws an icon at x (y a row's baseline) when draw, and returns its width with the gap after it (0: none). The Tower's
// emblems (tower.c sets it before it fills a doc with them)
extern int (*ui_icon_hook)(uint8_t icon, int x, int y, bool draw);
int  ui_doc_layout(UiDoc *d, int w, int h);     // returns the pages (1 at least)
void ui_doc_draw(const UiDoc *d, int page, int x, int y, int w);     // y: the first line's baseline
int  ui_line_pitch(void);               // the font's line pitch (FONT_UNBALANCED)
int  ui_text_width(const char *s);      // one line's width in pixels
#define UI_TOO_WIDE 99
int  ui_text_lines(const char *s, int w);   // its lines, word-wrapped to w (UI_TOO_WIDE if a word is wider than w)
int  ui_text_lines_cs(const char *s, int w, int cs);     // the same with cs px between glyphs (-1: set tight)

typedef struct { int8_t page, pages, sel; } UiPager;   // sel: 0 the left corner, 1 the right
enum { UI_PAGER_NONE, UI_PAGER_TURNED, UI_PAGER_BACK };
void ui_pager_reset(UiPager *p, int pages);     // page 0, the cursor on Next (Back on a lone page)
int  ui_pager_input(UiPager *p, bool left, bool right, bool a, bool b);
void ui_pager_draw(const UiPager *p, int x, int y, int w, const char *title);     // the corners, the title and "2/3" between; y: the row's baseline

// THE MENU STANDARD (one look and one way to move in every menu and screen, 2026-11-03):
//   the box: one box for every UI (the user's pick, 2026-11-03, the "hybrid"): rounded corners with a soft rim, a
//   9-slice of one 16x16 I4 corner (it renders on Delta's renderer as on Mupen's and Ares'), translucent black at one
//   shade, UI_SHADE (180: the scene still shows through, the text legible over the title's fire, the Sun and the
//   hall's light), no border. Three widths: UI_NARROW for the short centred menus (pause, settings, audio, video, controls,
//   Bonus, Tower, credits), the safe rect's (UI_WIDE) for the lists, Details, the paged screens and the Tower's list
//   screens, the screen's for the full screens; its height fits its content with one padding, UI_EVEN (ui_fit: measured
//   in a first pass, then drawn), centred in the title-safe rect (ui_safe_rect: the overscan setting's, never wider than
//   the fixed 7.5% one); paged screens (Stats/Build, the Codex) keep the safe rect's height, a Details panel its tallest
//   page. Only the Tower's full-screen screens (the draft, the map, the
//   Shop) take the screen's size; their cards and buttons the same black. Its title: uppercase, white, centred on its first row, UI_EVEN under the box's top (a pager's
//   corners on that row too). Rows: a list's cursor is ui_select_box; a row of buttons (Enter | Back, the Shop's,
//   Skip) is ui_button. Colours: white, grey (can't be chosen, or a hint), gold (owned, money, set), red
//   (why not), the families' own. The footer: the box's last row (UI_FOOT_DY over its bottom), "Details (R)"
//   right-aligned in grey where R has more to read; a row's line of help over it, centred grey.
//   Input: D-pad and stick move, A takes the row, B goes back a level (the Tower's draft ignores B: it's the
//   sword in the Classic layout), every screen has a Back row (or Leave, Keep them all, Continue, Resume, a
//   pager's Back corner) A alone reaches; Left/Right turn ui_pages' pages; R opens Details. No button letters in
//   any prompt: "Enter", "Back", "Close", "Continue" (R in parentheses: "Details (R)").
#define UI_TITLE_DY 16
#define UI_SHADE    180     // the menus' black (its alpha), everywhere
#define UI_FOOT_DY  6
#define UI_PAD      12      // the box past its content's rect, every side (ui_box)
typedef struct { int x0, y0, x1, y1; } UiRect;
UiRect ui_safe_rect(void);
// Boxes fit to their content (one even padding, UI_EVEN, between the box's edge and what's in it): a screen is drawn
// twice a frame, first measured (ui_measure_begin/end: an empty scissor, nothing reaches the screen; its text, cursors and
// buttons tracked as they're laid out, the footer's apart: ui_track_foot), then in the rect ui_fit gives (its content's
// size, centred in the safe rect; a list whose rows reach the safe rect's sides keeps its width). Text goes through ui_textf
// (rdpq_text_printf's twin: the files that draw menus #define rdpq_text_printf ui_textf), which tracks what it lays out
#include <rdpq_text.h>
rdpq_textmetrics_t ui_textf(const rdpq_textparms_t *parms, uint8_t font, float x, float y, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
#define UI_EVEN 14      // the padding round a box's content, every side
#define UI_NARROW 216   // a box's widths: the short menus (pause, settings), the rest the safe rect's (UI_WIDE), the full screens the screen's
#define UI_WIDE 0
void ui_track(int x0, int y0, int x1, int y1);
void ui_track_foot(bool foot);          // what's tracked next is the footer's (anchored to the box's bottom)
void ui_measure_begin(void);
void ui_measure_end(void);
bool ui_measuring(void);
UiRect ui_fit(UiRect drawn, int width, UiRect *box);     // the rect to lay out in again and the box, from what was measured in `drawn` (the safe rect)
UiRect ui_tracked(bool foot);           // what the last pass laid out (the self-checks: inside its box less UI_EVEN)
UiRect ui_last_box(void);               // the last box ui_box drew (its rect on screen)
void ui_box(int x0, int y0, int x1, int y1);            // UI_PAD round the rect
void ui_box_exact(int x0, int y0, int x1, int y1);      // the box itself
void ui_frame(int x0, int y0, int x1, int y1, int t);     // a frame t px wide in the colour set (a translucent fill inside shows no colour through)
// the menus' selection box (black 140 under a 1 px white 70 frame)
void ui_select_box(int x0, int y0, int x1, int y1);
// a framed button (gold when it's under the cursor, grey text when it can't be taken); y its top, h its height
void ui_button(int x, int y, int w, int h, bool on, bool ok, const char *label);
// a small grey hint ("Details (R)") ending at x (right-aligned), y its baseline
void ui_hint(int xRight, int y, const char *s);
#define UI_DETAILS_HINT "Details (R)"

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
