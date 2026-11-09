// Test aid (HLE_TEXT_CHECK): every string the game lays out (rdpq_text_printf, ui_text_width, ...),
// checked before libdragon sees it: only printable ASCII, '\n', the Tower's icon glyphs (\x01 gold,
// \x02 Embers) and well-formed UTF-8, and every '^' / '$' escape two hex digits (or doubled). A bad
// one logs "[textcheck] BAD" with its caller and its first bytes (the Heat row's stack garbage,
// 2026-11-06, drew blank in Mupen and asserted on Delta). Linked by --wrap (Makefile)
#ifdef HLE_TEXT_CHECK
#include <libdragon.h>
#include <stdint.h>

int hle_textcheck_bad;

static bool hexd(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }

static const char *text_problem(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '^' || c == '$') {
            if (i + 1 < n && (uint8_t)s[i + 1] == c) { i++; continue; }
            if (i + 2 >= n || !hexd(s[i + 1]) || !hexd(s[i + 2])) return c == '^' ? "a bad ^ style escape" : "a bad $ font escape";
            i += 2; continue;
        }
        if (c == '\n' || c == 0x01 || c == 0x02 || (c >= 0x20 && c < 0x7f)) continue;
        if (c >= 0xc2 && c <= 0xf4) {      // a UTF-8 sequence: its continuation bytes
            int k = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            if (i + k >= n) return "a cut UTF-8 sequence";
            for (int j = 1; j <= k; j++) if (((uint8_t)s[i + j] & 0xc0) != 0x80) return "bad UTF-8";
            i += k; continue;
        }
        return "a control or stray byte";
    }
    return NULL;
}

static void check(const char *s, int n, void *caller)
{
    const char *why = text_problem(s, n);
    if (!why) return;
    if (hle_textcheck_bad++ < 20) {
        char hex[3 * 16 + 1] = { 0 };
        for (int i = 0; i < n && i < 16; i++) snprintf(hex + 3 * i, 4, "%02x ", (uint8_t)s[i]);
        debugf("[textcheck] BAD: %s, %d bytes from %p: %s\n", why, n, caller, hex);
    }
}

struct rdpq_paragraph_s;
struct rdpq_paragraph_s *__real___rdpq_paragraph_build(const void *parms, uint8_t font, const char *utf8, int *nbytes, struct rdpq_paragraph_s *layout);
struct rdpq_paragraph_s *__wrap___rdpq_paragraph_build(const void *parms, uint8_t font, const char *utf8, int *nbytes, struct rdpq_paragraph_s *layout)
{
    check(utf8, *nbytes, __builtin_return_address(0));
    return __real___rdpq_paragraph_build(parms, font, utf8, nbytes, layout);
}
struct rdpq_paragraph_s *__real_rdpq_paragraph_build(const void *parms, uint8_t font, const char *utf8, int *nbytes);
struct rdpq_paragraph_s *__wrap_rdpq_paragraph_build(const void *parms, uint8_t font, const char *utf8, int *nbytes)
{
    check(utf8, *nbytes, __builtin_return_address(0));
    return __real_rdpq_paragraph_build(parms, font, utf8, nbytes);
}

#ifdef HLE_TEXT_CHECK_SELFTEST     // the check's own test: a stray byte, a bad escape, then good text
void hle_textcheck_selftest(void)
{
    static const char a[] = "Heat\x05", b[] = "x^zz", c[] = "Heat: off \x02 12 ^^ $$";
    check(a, sizeof a - 1, 0); check(b, sizeof b - 1, 0); int before = hle_textcheck_bad; check(c, sizeof c - 1, 0);
    debugf("[textcheck] selftest: %d bad of the 2 bad strings, the good one %s\n", before, hle_textcheck_bad == before ? "passed" : "FLAGGED");
}
#endif
#endif
