/**
 * Virtual RDP: receives the RDP command stream that libdragon (rdpq, Tiny3D, ...)
 * produces, and re-encodes it as an F3DEX2 display list. RDP state commands are
 * native F3DEX2 commands too, so they pass through; texture rectangles get the
 * 3-word GBI encoding; geometry is emitted by the overlays as G_VTX/G_TRI.
 *
 * At every SYNC_FULL the list is submitted to the RSP as a standard OSTask,
 * which HLE emulators hand to their graphics plugin.
 */
#include <libdragon.h>
#include <stdarg.h>
#include <string.h>
#include "hle.h"
#include "gbi.h"

#define DL_CMDS        (24 * 1024)      // commands per task
#define SCRATCH_BYTES  (256 * 1024)     // vertices/matrices per task

static uint64_t *dl_base;
static uint8_t *scratch_base;
uint64_t *__gbi_ptr, *__gbi_end;
uint8_t *__gbi_scratch, *__gbi_scratch_end;
#define dl_ptr      __gbi_ptr
#define dl_end      __gbi_end
#define scratch_ptr __gbi_scratch
#define scratch_end __gbi_scratch_end
uint32_t gsp_epoch, gsp_tasks;

static uint32_t dp_cur;                 // virtual DP_CURRENT (physical)

// Last RDP state seen, needed to restore after internal commands.
static uint64_t last_cimg, last_som = 0xEF00000000000000ull, last_fillcolor = 0xF700000000000000ull;
static uint64_t last_scissor = 0xED00000000000000ull;
// Other modes / combiner the plugin has this task: models set the same ones over and over
static uint64_t sent_som = ~0ull, sent_comb = ~0ull;

gsp_state_t gsp_3d;
static struct {
    uint32_t proj_ver, vp_ver, geom, tex, fog;
    bool valid;                         // false after a task split or a 2D draw
} gsp_cur;

// ---- Logging ----

void hle_logf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    debugf("[hle] %s\n", buf);
}

void hle_warn_once(const char *key, const char *fmt, ...)
{
    static const char *seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == key) return;
        if (!seen[i]) { seen[i] = key; break; }
    }
    va_list va;
    va_start(va, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    debugf("[hle] WARNING: %s\n", buf);
}

// ---- Task submission ----

// HLE emulators identify the graphics microcode from the version string in its
// data segment; nothing in the dummy text segment is ever executed.
static const char ucode_id[] = "RSP Gfx ucode F3DEX       fifo 2.08  Yoshitake Matsumoto 1999 Nintendo.";
static uint8_t ucode_text[4096] __attribute__((aligned(16)));
static uint8_t ucode_data[2048] __attribute__((aligned(16)));
static uint8_t ucode_stack[1024] __attribute__((aligned(16)));
static uint8_t ucode_yield[256] __attribute__((aligned(16)));

static void task_submit(void)
{
    hle_t3d_flush();
    *dl_ptr++ = (uint64_t)G_ENDDL << 56;
    uint32_t size = (uint8_t*)dl_ptr - (uint8_t*)dl_base;

    // HLE emulators raise the DP interrupt twice per FULLSYNC (graphics plugin +
    // core), which confuses rdpq's handler; SYNC_FULL completion is emulated
    // synchronously instead (ovl_rdpq.c), so keep it masked.
    set_DP_interrupt(0);
    // Nothing else ever runs on the RSP in this build: just make sure it's halted.
    *SP_STATUS = SP_WSTATUS_SET_HALT;
    volatile uint32_t *t = (volatile uint32_t*)0xA4000FC0;   // OSTask in DMEM
    t[0]  = 1;                              // M_GFXTASK
    t[1]  = 0;                              // flags
    t[2]  = PhysicalAddr(ucode_text);       // ucode_boot
    t[3]  = 0x80;                           // ucode_boot_size (must be <= 0x1000)
    t[4]  = PhysicalAddr(ucode_text);       // ucode
    t[5]  = sizeof(ucode_text);
    t[6]  = PhysicalAddr(ucode_data);       // ucode_data (version string lives here)
    t[7]  = sizeof(ucode_data);
    t[8]  = PhysicalAddr(ucode_stack);      // dram_stack (matrix stack)
    t[9]  = sizeof(ucode_stack);
    t[10] = 0;                              // output_buff
    t[11] = 0;
    t[12] = PhysicalAddr(dl_base);          // data_ptr: the display list
    t[13] = size;
    t[14] = PhysicalAddr(ucode_yield);
    t[15] = sizeof(ucode_yield);
    MEMORY_BARRIER();
    // The emulator executes the whole task inside this register write (and,
    // without interrupt-on-break, never reports completion: nothing to wait for).
    *SP_STATUS = SP_WSTATUS_CLEAR_INTR_BREAK | SP_WSTATUS_CLEAR_SSTEP |
                 SP_WSTATUS_CLEAR_BROKE | SP_WSTATUS_CLEAR_HALT;
    MEMORY_BARRIER();

#ifdef HLE_TEST_TASKS
    extern int hle_task_log;
    if (hle_task_log) {
        hle_logf("task %lu bytes, first cmd %08lx", (unsigned long)size, (uint32_t)(dl_base[0] >> 32));
        int n = dl_ptr - dl_base;
        for (int i = 0; i < n; i++) if (n < 50 || i < 14 || i >= n - 8) debugf("  %08lx %08lx\n", (uint32_t)(dl_base[i] >> 32), (uint32_t)dl_base[i]);
    }
#endif
#ifdef HLE_DUMP_TASKS
    static int ntasks;
    if (ntasks++ < HLE_DUMP_TASKS) {
        hle_logf("task %d: %ld bytes", ntasks - 1, size);
        for (uint64_t *c = dl_base; c < dl_ptr; c++)
            debugf("  %08lx %08lx\n", (uint32_t)(*c >> 32), (uint32_t)*c);
    }
#endif

    dl_ptr = dl_base;
    scratch_ptr = scratch_base;
    sent_som = sent_comb = ~0ull;
    gsp_cur.valid = false;
    gsp_epoch++;
    gsp_tasks++;
}

void gbi_split(void)
{
    task_submit();
}

void gbi_mtx(const hle_mat4_t *mf, uint32_t which)
{
    gbi_mtx_t *m = gbi_alloc(sizeof(gbi_mtx_t));
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            int32_t v = (int32_t)((*mf)[c][r] * 65536.0f);
            m->i[c*4+r] = v >> 16;
            m->f[c*4+r] = v & 0xFFFF;
        }
    }
    GBI_MTX(PhysicalAddr(m), G_MTX_LOAD | G_MTX_NOPUSH | which);
}

void gsp_sync_3d(void)
{
    uint32_t geom = gsp_3d.fog ? gsp_3d.geom | G_FOG : gsp_3d.geom;
    if (gsp_cur.valid && gsp_cur.vp_ver == gsp_3d.vp_ver && gsp_cur.proj_ver == gsp_3d.proj_ver &&
        gsp_cur.geom == geom && gsp_cur.tex == gsp_3d.tex && (!gsp_3d.fog || gsp_cur.fog == gsp_3d.fog))
        return;
    gbi_reserve(9, 2*sizeof(gbi_mtx_t) + 16);
    bool all = !gsp_cur.valid;
    if (all || gsp_cur.vp_ver != gsp_3d.vp_ver) {
        int16_t *vp = gbi_alloc(16);
        memcpy(vp, gsp_3d.vp, 16);
        GBI_VIEWPORT(PhysicalAddr(vp));
        gsp_cur.vp_ver = gsp_3d.vp_ver;
    }
    if (gsp_3d.proj && (all || gsp_cur.proj_ver != gsp_3d.proj_ver)) {
        gbi_mtx(gsp_3d.proj, G_MTX_PROJECTION);
        gsp_cur.proj_ver = gsp_3d.proj_ver;
    }
    if (gsp_3d.fog && (all || gsp_cur.fog != gsp_3d.fog)) {
        GBI_FOG(gsp_3d.fog >> 16, gsp_3d.fog);
        gsp_cur.fog = gsp_3d.fog;
    }
    if (all || gsp_cur.geom != geom) {
        GBI_GEOMETRY(0xFFFFFFFF, geom);
        gsp_cur.geom = geom;
    }
    if (all || gsp_cur.tex != gsp_3d.tex) {
        gbi(((uint32_t)G_TEXTURE << 24) | gsp_3d.tex, 0xFFFFFFFF);
        gsp_cur.tex = gsp_3d.tex;
    }
    gsp_cur.valid = true;
}

static int cimg_width(void) { return ((last_cimg >> 32) & 0x3FF) + 1; }

void gsp_sync_2d(uint32_t geom, uint32_t tex)
{
    hle_t3d_flush();
    gbi_reserve(8, 2*sizeof(gbi_mtx_t) + 16);
    // Pixel space (x4 for sub-pixel precision) to clip space.
    int w = cimg_width();
    int h = ((last_scissor & 0xFFF) >> 2);
    if (h <= 0) h = 240;
    float sx = 2.0f / (w * 4), sy = -2.0f / (h * 4);
    float mf[4][4] = {{sx,0,0,0},{0,sy,0,0},{0,0,1,0},{-1,1,0,1}};
    gbi_mtx_t *m = gbi_alloc(sizeof(gbi_mtx_t));
    gbi_mtx_t *id = gbi_alloc(sizeof(gbi_mtx_t));
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        int32_t v = (int32_t)(mf[r][c] * 65536.0f);
        m->i[r*4+c] = v >> 16; m->f[r*4+c] = v & 0xFFFF;
        id->i[r*4+c] = (r == c); id->f[r*4+c] = 0;
    }
    int16_t *vp = gbi_alloc(16);
    int16_t v[8] = { w*2, h*2, 0x1FF, 0, w*2, h*2, 0x1FF, 0 };
    memcpy(vp, v, 16);
    GBI_VIEWPORT(PhysicalAddr(vp));
    GBI_MTX(PhysicalAddr(m), G_MTX_LOAD | G_MTX_PROJECTION | G_MTX_NOPUSH);
    GBI_MTX(PhysicalAddr(id), G_MTX_LOAD | G_MTX_MODELVIEW | G_MTX_NOPUSH);
    GBI_GEOMETRY(0xFFFFFFFF, geom);
    gbi(((uint32_t)G_TEXTURE << 24) | tex, 0xFFFFFFFF);
    gsp_cur.valid = false;
    gsp_epoch++;
}

// ---- RDP command translation ----

static int tri_size(int op)
{
    int sz = 32;
    if (op & 4) sz += 64;   // shade
    if (op & 2) sz += 64;   // texture
    if (op & 1) sz += 16;   // z-buffer
    return sz;
}

/** Translates one RDP command; returns its size in bytes. */
static int translate(const uint64_t *c)
{
    hle_t3d_flush();
    uint64_t c0 = c[0];
    uint32_t w0 = c0 >> 32, w1 = (uint32_t)c0;
    int op = (w0 >> 24) & 0x3F;

    switch (op) {
    case 0x00:              // NOP (also reserved-but-unused rdpq slots)
        return 8;
    case 0x08 ... 0x0F:
        hle_warn_once("rawtri", "raw RDP triangle (op %02x) dropped", op);
        return tri_size(op);
    case 0x24: case 0x25:   // TEXTURE_RECTANGLE (+FLIP): GBI wants 3 words
        gbi_reserve(3, 0);
        gbi(w0 | 0xC0000000, w1);
        gbi((uint32_t)G_RDPHALF_1 << 24, c[1] >> 32);
        gbi((uint32_t)G_RDPHALF_2 << 24, (uint32_t)c[1]);
        return 16;
    case 0x29:              // SYNC_FULL: close and submit the task
        gbi((uint32_t)G_RDPFULLSYNC << 24, 0);
        task_submit();
        return 8;
    case 0x31:              // rdpq debug command (collides with G_RDPHALF_2)
        return 8;
    case 0x26 ... 0x28: case 0x2A ... 0x30: case 0x32 ... 0x3F:
        // rdpq "detaches" by pointing the color/Z image past the end of RDRAM
        // (RDPQ_VALIDATE_DETACH_ADDR). GLideN64 sizes those buffers to 0 rows,
        // which GLES (Delta on iOS) rejects with a fatal error: drop it.
        if ((op == 0x3E || op == 0x3F) && (w1 & 0xFFFFFF) >= (uint32_t)get_memory_size())
            return 8;
        gbi_reserve(1, 0);
        // rdpq keeps the image height in bits 10-18 of SET_TEXTURE/COLOR_IMAGE
        // (ignored by the RDP); GBI decoders read a 12-bit width there.
        if (op == 0x3D || op == 0x3F) {
            w0 &= ~0x0007FC00;
            c0 = ((uint64_t)w0 << 32) | w1;
        }
        if (op == 0x2F || op == 0x3C) {
            uint64_t *sent = op == 0x2F ? &sent_som : &sent_comb;
            if (*sent == c0) {
#ifdef HLE_TEST_PERF
                extern uint32_t hle_rm_calls[4]; hle_rm_calls[3]++;
#endif
                return 8;
            }
            *sent = c0;
        }
        switch (op) {
        case 0x3F: last_cimg = c0; break;
        case 0x2F: last_som = c0; break;
        case 0x37: last_fillcolor = c0; break;
        case 0x2D: last_scissor = c0; break;
        }
        gbi(w0 | 0xC0000000, w1);
        return 8;
    default:
        hle_warn_once("rdpop", "unknown RDP op %02x", op);
        return 8;
    }
}

void vrdp_set_start(uint32_t phys)
{
    dp_cur = phys & 0xFFFFFF;
}

void vrdp_set_end(uint32_t phys)
{
    phys &= 0xFFFFFF;
    while (dp_cur < phys)
        dp_cur += translate(hle_rdram(dp_cur));
}

void vrdp_sync_cursor(uint32_t phys)
{
    phys &= 0xFFFFFF;
    if (dp_cur == phys) return;
    hle_warn_once("dpcur", "virtual DP cursor resync %06lx -> %06lx", dp_cur, phys);
    dp_cur = phys;
}

void vrdp_direct(const uint64_t *cmds, int nbytes)
{
    const uint8_t *p = (const uint8_t*)cmds, *end = p + nbytes;
    while (p < end)
        p += translate((const uint64_t*)p);
    dp_cur += nbytes;
}

/** Z-buffer clear (rdpq does it with an RSP DMA; HLE needs a fill on the depth image). */
void vrdp_clear_z(uint32_t zaddr, uint32_t size, uint16_t value)
{
    hle_t3d_flush();
    int w = cimg_width();
    int h = size / (w * 2);
    gbi_reserve(10, 0);
    gbi(0xFF100000 | (w - 1), zaddr & 0xFFFFFF);                // CIMG = zbuf, RGBA16
    gbi(0xED000000, ((w << 2) << 12) | (h << 2));              // full scissor
    gbi(0xEF300000, 0);                                         // FILL cycle
    gbi(0xF7000000, ((uint32_t)value << 16) | value);
    gbi(0xF6000000 | (((w - 1) << 2) << 12) | ((h - 1) << 2), 0);
    gbi(0xE7000000, 0);                                         // SYNC_PIPE
    gbi(last_cimg >> 32, (uint32_t)last_cimg);
    gbi(last_scissor >> 32, (uint32_t)last_scissor);
    gbi(last_som >> 32, (uint32_t)last_som);
    gbi(last_fillcolor >> 32, (uint32_t)last_fillcolor);
    sent_som = last_som;
}

void vrdp_init(void)
{
    if (dl_base) return;
    debug_init_isviewer();
    hle_logf("HLE backend init");
    dl_base = malloc_uncached(DL_CMDS * 8);
    scratch_base = malloc_uncached(SCRATCH_BYTES);
    assertf(dl_base && scratch_base, "HLE: out of memory");
    dl_ptr = dl_base; dl_end = dl_base + DL_CMDS;
    scratch_ptr = scratch_base; scratch_end = scratch_base + SCRATCH_BYTES;
    memcpy(ucode_data, ucode_id, sizeof(ucode_id));
    data_cache_hit_writeback(ucode_data, sizeof(ucode_data));
    // Plain viewport until Tiny3D sets one
    int16_t vp[8] = { 320*2, 240*2, 0x1FF, 0, 320*2, 240*2, 0x1FF, 0 };
    memcpy(gsp_3d.vp, vp, 16);
}

// Route libdragon's debug output (asserts, crash inspector) to the emulator's
// IS-Viewer console as early as possible (after the PI/DMA constructor, 130).
__attribute__((constructor(131))) static void hle_early_debug(void)
{
    debug_init_isviewer();
}

// ---- Block replay: this module's state ----

void hle_vrdp_state(hle_st_t *s, bool sig)
{
    // the color image cycles through the framebuffers: blocks only depend on its format and width,
    // and leave it as it was unless they set one
    static uint64_t cimg_entry;
    if (sig) {
        uint32_t fmt = last_cimg >> 32;
        HLE_ST(s, fmt);
        cimg_entry = last_cimg;
    } else {
        uint8_t changed = last_cimg != cimg_entry;
        uint64_t v = changed ? last_cimg : 0;
        HLE_ST(s, changed); HLE_ST(s, v);
        if (s->load && changed) last_cimg = v;
    }
    HLE_ST(s, last_som); HLE_ST(s, last_fillcolor); HLE_ST(s, last_scissor);
    HLE_ST(s, sent_som); HLE_ST(s, sent_comb);
    HLE_ST(s, gsp_3d.proj); HLE_ST(s, gsp_3d.vp); HLE_ST(s, gsp_3d.geom); HLE_ST(s, gsp_3d.tex); HLE_ST(s, gsp_3d.fog);
    HLE_ST(s, gsp_cur.geom); HLE_ST(s, gsp_cur.tex); HLE_ST(s, gsp_cur.fog); HLE_ST(s, gsp_cur.valid);
    // versions as steps behind the current ones
    uint32_t rel[2] = { gsp_cur.proj_ver - gsp_3d.proj_ver, gsp_cur.vp_ver - gsp_3d.vp_ver };
    HLE_ST(s, rel);
    if (s->load) { gsp_cur.proj_ver = gsp_3d.proj_ver + rel[0]; gsp_cur.vp_ver = gsp_3d.vp_ver + rel[1]; }
}
void hle_vrdp_counters(uint32_t *epoch, uint32_t *proj_ver, uint32_t *vp_ver)
{
    *epoch = gsp_epoch; *proj_ver = gsp_3d.proj_ver; *vp_ver = gsp_3d.vp_ver;
}
void hle_vrdp_counters_set(uint32_t epoch, uint32_t proj_ver, uint32_t vp_ver)
{
    gsp_epoch = epoch; gsp_3d.proj_ver = proj_ver; gsp_3d.vp_ver = vp_ver;
}
uint32_t hle_vrdp_dp_cur(void) { return dp_cur; }
void hle_vrdp_dp_cur_set(uint32_t phys) { dp_cur = phys & 0xFFFFFF; }
