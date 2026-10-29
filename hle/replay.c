/**
 * Block replay: the GBI a recorded rspq block turns into, saved once and reused.
 *
 * A block (rspq_block_begin/end, recorded once at load) is translated every time
 * it runs, but its output only depends on its commands (static), the state it
 * starts from and the matrices it reads (rewritten every frame). So the first
 * run with a given starting state is recorded: the display list it emitted, the
 * scratch memory it used (relocated on replay), where each matrix came from (re-
 * converted on replay) and the state it left. A later run starting from the same
 * state copies all that instead of interpreting the block.
 *
 * Anything a recording can't reproduce makes the block unreplayable for that
 * run: CPU fog, vertices or strips outside models (rewritten every frame),
 * particles, signals, DMA, a task split, a highpri switch, DMEM writes outside
 * an overlay's state.
 *
 * HLE_TEST_REPLAY_CHECK runs blocks normally and compares each run with its
 * recording: the display list, scratch, matrix sources and the state left must
 * all be identical.
 */
#include <libdragon.h>
#include <string.h>
#include "hle.h"
#include "gbi.h"

#define RP_BLOCKS   512             // power of 2
#define RP_RECS     4               // recordings per block (one per starting state)
#define RP_SIG_MAX  8192
#define RP_SEG      15              // GBI segment the recorded lists reach their scratch through

typedef struct {
    uint32_t off;                   // scratch offset of the converted matrix
    uint32_t src;                   // entry slot (RP_SRC_SLOT) or the address the block gave (segmented)
    uint32_t kind;
} rp_mtx_t;
#define RP_SRC_SLOT 1
#define RP_SRC_RAW  2

typedef struct {
    uint32_t sig_len, exit_len;
    uint32_t ncmds, scratch_bytes, nmtx, nreloc;
    uint32_t vc_gen;
    uint32_t ovl_mask;              // overlays whose state is in the signature
    // rdp cursor: relative bytes sent before the first absolute buffer switch
    uint32_t rdp_rel;
    bool rdp_abs;
    uint32_t rdp_cur, rdp_sentinel, rdp_dp; uint8_t rdp_syncfull;
    uint32_t lru;
    uint8_t *sig, *exit;            // starting / final state
    uint64_t *dl;                   // w1 of relocated commands holds the scratch offset
    uint64_t *gdl;                  // the list as the plugin calls it (uncached; scratch through RP_SEG)
    uint32_t nspan; uint32_t *span; // scratch byte ranges that aren't matrices (copied as recorded)
    uint32_t *reloc;                // command indices
    uint8_t *scratch;
    rp_mtx_t *mtx;
} rp_rec_t;

typedef struct { uint32_t addr; rp_rec_t *rec[RP_RECS]; uint16_t skip, misses; } rp_block_t;

static rp_block_t rp_blocks[RP_BLOCKS];
static uint32_t rp_lru;

// The recording in progress
static struct {
    bool on, bad;
    int slot;                       // pointer stack slot of the block's CALL
    rp_block_t *blk;
    uint64_t *dl0;
    uint8_t *sc0;
    uint32_t tasks;
    uint32_t ovl_mask;
    rp_mtx_t mtx[512]; int nmtx;
    uint32_t rdp_rel; bool rdp_abs;
    uint8_t sig[RP_SIG_MAX]; uint32_t sig_len;
    uint32_t e_entry, p3_entry, v3_entry;
    uint16_t slots_written;
    uint32_t pstack_written;
    int zmin;
#ifdef HLE_TEST_REPLAY_CHECK
    rp_rec_t *check;                // the recording this run must match
#endif
} rp;

#if defined(HLE_TEST_PERF) || defined(HLE_TEST_REPLAY_CHECK)
uint32_t hle_rp_stats[8];           // replays, recorded, unreplayable, no match, check ok, check bad
#endif

// ---- Registry ----

static rp_block_t *rp_find(uint32_t addr, bool add)
{
    uint32_t h = (addr >> 2) * 2654435761u >> 23;
    for (int n = 0; n < RP_BLOCKS; n++, h = (h + 1) & (RP_BLOCKS - 1)) {
        rp_block_t *b = &rp_blocks[h];
        if (b->addr == addr) return b;
        if (!b->addr) {
            if (!add) return NULL;
            b->addr = addr;
            return b;
        }
    }
    return NULL;
}

// Freed recordings wait until the plugin is done with the task that may still call their list
static struct { rp_rec_t *r; uint32_t task; } rp_dead[256];
static int rp_ndead;
static void rp_rec_destroy(rp_rec_t *r) { free_uncached(r->gdl); free(r->span); free(r->sig); free(r); }
static void rp_collect(void)
{
    int n = 0;
    for (int i = 0; i < rp_ndead; i++) {
        if (rp_dead[i].task != gsp_tasks) rp_rec_destroy(rp_dead[i].r);
        else rp_dead[n++] = rp_dead[i];
    }
    rp_ndead = n;
}
static void rp_rec_free(rp_rec_t *r)
{
    if (!r) return;
    if (rp_ndead == 256) { rp_rec_destroy(rp_dead[0].r); memmove(rp_dead, rp_dead + 1, sizeof(rp_dead[0]) * 255); rp_ndead--; }
    rp_dead[rp_ndead].r = r; rp_dead[rp_ndead].task = gsp_tasks; rp_ndead++;
}

void hle_replay_block_new(const void *cmds)
{
    rp_block_t *b = rp_find(PhysicalAddr(cmds), true);
    if (!b) return;
    for (int i = 0; i < RP_RECS; i++) { rp_rec_free(b->rec[i]); b->rec[i] = NULL; }
}

void hle_replay_block_gone(const void *cmds)
{
    rp_block_t *b = rp_find(PhysicalAddr(cmds), false);
    if (!b) return;
    if (rp.on && rp.blk == b) rp.bad = true;
    for (int i = 0; i < RP_RECS; i++) { rp_rec_free(b->rec[i]); b->rec[i] = NULL; }
    // keep the slot (a tombstone with no recordings), so probes for others still find them
}

// ---- Hooks ----

bool hle_rp_on;
void hle_rp_bad_(void) { rp.bad = true; }
void hle_rp_slot_written_(int k) { rp.slots_written |= 1u << k; }
void hle_rp_pstack_(int slot) { if (slot > rp.slot) rp.pstack_written |= 1u << slot; }
void hle_rp_zvalid_(int v) { if (v < rp.zmin) rp.zmin = v; }
void hle_rp_rdp_send_(int nbytes) { if (!rp.rdp_abs) rp.rdp_rel += nbytes; }
void hle_rp_rdp_abs_(void) { rp.rdp_abs = true; }

/** A matrix converted into scratch at `dst`: from slot k's matrix (entry value unless the block set it). */
void hle_rp_mtx_(const void *dst, int k, uint32_t raw)
{
    if (rp.nmtx == (int)(sizeof(rp.mtx) / sizeof(rp.mtx[0]))) { rp.bad = true; return; }
    bool entry = !(rp.slots_written & (1u << k));
    rp.mtx[rp.nmtx++] = entry ? (rp_mtx_t){ (const uint8_t*)dst - rp.sc0, k, RP_SRC_SLOT }
                              : (rp_mtx_t){ (const uint8_t*)dst - rp.sc0, raw, RP_SRC_RAW };
}

// ---- State ----

/** Everything a block's output depends on (normalized: no absolute counters or cursors). */
static uint32_t rp_signature(uint8_t *out, uint32_t ovl_mask)
{
    hle_st_t s = { out, false };
    hle_t3d_state(&s, true);
    hle_rdpq_state(&s);
    hle_vrdp_state(&s, true);
    hle_vrsp_state(&s, ovl_mask, true);
    return s.p - out;
}

/** The state left: everything above plus what only matters after (written slots, pointer stack). */
static uint32_t rp_exit_io(uint8_t *buf, bool load, uint32_t ovl_mask)
{
    hle_st_t s = { buf, load };
    hle_t3d_state(&s, false);
    hle_rdpq_state(&s);
    hle_vrdp_state(&s, false);
    hle_vrsp_state(&s, ovl_mask, false);
    return s.p - buf;
}

// ---- Recording ----

static uint8_t rp_exit_buf[RP_SIG_MAX + 1024];

static void rp_begin(rp_block_t *b, int slot)
{
    rp.on = hle_rp_on = true; rp.bad = false;
#ifdef HLE_TEST_REPLAY_CHECK
    rp.check = NULL;
#endif
    rp.blk = b; rp.slot = slot;
    rp.dl0 = __gbi_ptr; rp.sc0 = __gbi_scratch;
    rp.tasks = gsp_tasks;
    rp.ovl_mask = hle_vrsp_loaded_mask();
    rp.nmtx = 0;
    rp.rdp_rel = 0; rp.rdp_abs = false;
    rp.slots_written = 0; rp.pstack_written = 0;
    rp.zmin = 1 << 30;
    hle_vrdp_counters(&rp.e_entry, &rp.p3_entry, &rp.v3_entry);
    if (!hle_vrsp_rdp_synced()) rp.bad = true;
    rp.sig_len = rp_signature(rp.sig, rp.ovl_mask);
    assertf(rp.sig_len <= RP_SIG_MAX, "HLE replay: signature of %lu bytes", rp.sig_len);
}

/** The block returned: keep what it did as a recording (or compare it with the one it must match). */
static void rp_end(void)
{
    rp.on = hle_rp_on = false;
    hle_t3d_flush();
    if (rp.bad || gsp_tasks != rp.tasks || hle_vrsp_loaded_mask() != rp.ovl_mask) {
#ifdef HLE_TEST_REPLAY_CHECK
        rp.check = NULL;
#endif
        // try this block again less and less often (cloth, per-frame meshes are never replayable)
        if (rp.blk->misses < 6) rp.blk->misses++;
        rp.blk->skip = (1 << rp.blk->misses) - 1;
#if defined(HLE_TEST_PERF) || defined(HLE_TEST_REPLAY_CHECK)
        hle_rp_stats[2]++;
#endif
        return;
    }
    uint32_t ncmds = __gbi_ptr - rp.dl0, sbytes = __gbi_scratch - rp.sc0;
    // counters (as steps from the entry) first: the rest is stored relative to them
    uint32_t e, p3, v3; hle_vrdp_counters(&e, &p3, &v3);
    uint32_t extra[6] = { e - rp.e_entry, p3 - rp.p3_entry, v3 - rp.v3_entry, (uint32_t)rp.zmin, rp.slots_written, rp.pstack_written };
    memcpy(rp_exit_buf, extra, sizeof(extra));
    uint32_t elen = sizeof(extra);
    elen += rp_exit_io(rp_exit_buf + elen, false, rp.ovl_mask);
    for (int k = 0; k < 16; k++) if (rp.slots_written & (1u << k)) elen += hle_t3d_slot_io(rp_exit_buf + elen, k, false);
    for (int k = 0; k < 32; k++) if (rp.pstack_written & (1u << k)) elen += hle_vrsp_pstack_io(rp_exit_buf + elen, k, false);

    // scratch pointers in the list become offsets
    uint32_t s0 = PhysicalAddr(rp.sc0), s1 = s0 + sbytes, nreloc = 0;
    static uint32_t relocs[24 * 1024];
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t w0 = rp.dl0[i] >> 32, w1 = rp.dl0[i], op = w0 >> 24;
        if ((op == G_VTX || op == G_MTX || op == G_MOVEMEM || op == G_DL) && w1 >= s0 && w1 < s1) {
            if (nreloc == sizeof(relocs) / 4) { rp.bad = true; break; }
            relocs[nreloc++] = i;
        }
    }
    uint32_t rcur, rsent, rdp; uint8_t rsync;
    hle_vrsp_rdp_get(&rcur, &rsent, &rdp, &rsync);

#ifdef HLE_TEST_REPLAY_CHECK
    if (rp.check) {
        rp_rec_t *r = rp.check;
        rp.check = NULL;
        bool ok = !rp.bad && r->ncmds == ncmds && r->scratch_bytes == sbytes && r->nmtx == (uint32_t)rp.nmtx && r->nreloc == nreloc &&
                  r->exit_len == elen && !memcmp(r->exit, rp_exit_buf, elen) && !memcmp(r->reloc, relocs, nreloc * 4) &&
                  !memcmp(r->mtx, rp.mtx, rp.nmtx * sizeof(rp_mtx_t)) && r->rdp_abs == rp.rdp_abs &&
                  (rp.rdp_abs ? (r->rdp_cur == rcur && r->rdp_sentinel == rsent && r->rdp_dp == rdp && r->rdp_syncfull == rsync) : r->rdp_rel == rp.rdp_rel);
        int bad_cmd = -1;
        if (ok) {
            for (uint32_t i = 0, ri = 0; i < ncmds; i++) {
                uint64_t c = rp.dl0[i];
                if (ri < nreloc && relocs[ri] == i) { c = (c & ~0xFFFFFFFFull) | ((uint32_t)c - s0); ri++; }
                if (c != r->dl[i]) { ok = false; bad_cmd = i; break; }
            }
        }
        if (ok) {   // scratch, apart from the matrices (rewritten every frame)
            static uint8_t mask[256 * 1024];
            if (sbytes <= sizeof(mask)) {
                memset(mask, 0, sbytes);
                for (int m = 0; m < rp.nmtx; m++) memset(mask + rp.mtx[m].off, 1, 64);
                for (uint32_t i = 0; i < sbytes; i++) if (!mask[i] && rp.sc0[i] != r->scratch[i]) { ok = false; bad_cmd = -2 - (int)i; break; }
            }
        }
        hle_rp_stats[ok ? 4 : 5]++;
        if (!ok) {
            static int logged;
            if (logged++ < 20) {
                int ediff = -1;
                for (uint32_t i = 0; i < elen && i < r->exit_len; i++) if (r->exit[i] != rp_exit_buf[i]) { ediff = i; break; }
                debugf("[replay] MISMATCH block %08lx: cmds %lu/%lu scratch %lu/%lu mtx %lu/%d reloc %lu/%lu exit %lu/%lu (first diff %d) rdp %d/%d %lu/%lu, cmd %d\n",
                       rp.blk->addr, r->ncmds, ncmds, r->scratch_bytes, sbytes, r->nmtx, rp.nmtx, r->nreloc, nreloc, r->exit_len, elen, ediff,
                       r->rdp_abs, rp.rdp_abs, r->rdp_rel, rp.rdp_rel, bad_cmd);
                if (bad_cmd >= 0) debugf("[replay]   was %016llx now %016llx\n", r->dl[bad_cmd], rp.dl0[bad_cmd]);
            }
        }
        return;
    }
#endif

    // store it in the block's least recently used slot
    rp_block_t *b = rp.blk;
    int victim = 0;
    for (int i = 0; i < RP_RECS; i++) {
        if (!b->rec[i]) { victim = i; break; }
        if (b->rec[i]->lru < b->rec[victim]->lru) victim = i;
    }
    uint32_t bytes = sizeof(rp_rec_t) + rp.sig_len + elen + ncmds * 8 + nreloc * 4 + sbytes + rp.nmtx * sizeof(rp_mtx_t) + 64;
    rp_rec_t *r = malloc(sizeof(rp_rec_t));
    uint8_t *mem = malloc(bytes);
    if (!r || !mem) { free(r); free(mem); return; }
    memset(r, 0, sizeof(*r));
    uint8_t *p = mem;
    r->sig = p; memcpy(p, rp.sig, rp.sig_len); p += (rp.sig_len + 7) & ~7;
    r->dl = (uint64_t*)p; p += ncmds * 8;
    r->exit = p; memcpy(p, rp_exit_buf, elen); p += (elen + 7) & ~7;
    r->reloc = (uint32_t*)p; memcpy(p, relocs, nreloc * 4); p += nreloc * 4;
    r->mtx = (rp_mtx_t*)p; memcpy(p, rp.mtx, rp.nmtx * sizeof(rp_mtx_t)); p += rp.nmtx * sizeof(rp_mtx_t);
    r->scratch = p; memcpy(p, rp.sc0, sbytes);
    for (uint32_t i = 0, ri = 0; i < ncmds; i++) {
        uint64_t c = rp.dl0[i];
        if (ri < nreloc && relocs[ri] == i) { c = (c & ~0xFFFFFFFFull) | ((uint32_t)c - s0); ri++; }
        r->dl[i] = c;
    }
    r->sig_len = rp.sig_len; r->exit_len = elen;
    r->ncmds = ncmds; r->scratch_bytes = sbytes; r->nmtx = rp.nmtx; r->nreloc = nreloc;
    r->vc_gen = hle_vc_gen();
    r->ovl_mask = rp.ovl_mask;
    r->rdp_rel = rp.rdp_rel; r->rdp_abs = rp.rdp_abs;
    r->rdp_cur = rcur; r->rdp_sentinel = rsent; r->rdp_dp = rdp; r->rdp_syncfull = rsync;
    r->lru = ++rp_lru;
    r->gdl = malloc_uncached((ncmds + 1) * 8);
    // the scratch that isn't matrices, copied as recorded
    static uint8_t is_mtx[64 * 1024];
    static uint32_t spans[1024];
    uint32_t nspan = 0;
    if (r->gdl && sbytes <= sizeof(is_mtx)) {
        memset(is_mtx, 0, sbytes);
        for (int m = 0; m < rp.nmtx; m++) memset(is_mtx + rp.mtx[m].off, 1, 64);
        for (uint32_t i = 0; i < sbytes && nspan < 1022; ) {
            if (is_mtx[i]) { i++; continue; }
            uint32_t j = i; while (j < sbytes && !is_mtx[j]) j++;
            spans[nspan++] = i; spans[nspan++] = j - i;
            i = j;
        }
    }
    r->span = (r->gdl && sbytes <= sizeof(is_mtx) && nspan < 1022) ? malloc(nspan * 4 + 4) : NULL;
    if (!r->span) { free_uncached(r->gdl); free(mem); free(r); return; }
    memcpy(r->span, spans, nspan * 4); r->nspan = nspan / 2;
    for (uint32_t i = 0; i < ncmds; i++) r->gdl[i] = r->dl[i];
    for (uint32_t i = 0; i < nreloc; i++) r->gdl[r->reloc[i]] |= (uint64_t)RP_SEG << 24;
    r->gdl[ncmds] = (uint64_t)G_ENDDL << 56;
    b->misses = 0;
    rp_rec_free(b->rec[victim]);
    b->rec[victim] = r;
#ifdef HLE_TEST_PERF
    hle_rp_stats[1]++;
#endif
}

// ---- Replay ----

static bool rp_replay(rp_rec_t *r)
{
    uint8_t *sc = gbi_alloc(r->scratch_bytes);
    for (uint32_t i = 0; i < r->nspan; i++) memcpy(sc + r->span[2*i], r->scratch + r->span[2*i], r->span[2*i+1]);
    for (uint32_t m = 0; m < r->nmtx; m++) {
        uint32_t src = r->mtx[m].kind == RP_SRC_SLOT ? hle_t3d_slot_addr(r->mtx[m].src) : hle_t3d_resolve(r->mtx[m].src);
        hle_t3d_mtx_conv_to(sc + r->mtx[m].off, src);
    }
#ifdef HLE_REPLAY_INLINE   // test aid: the list copied into the task instead of called
    { uint32_t s0 = PhysicalAddr(sc); uint64_t *d = __gbi_ptr;
      memcpy(d, r->dl, r->ncmds * 8); for (uint32_t i = 0; i < r->nreloc; i++) d[r->reloc[i]] += s0; __gbi_ptr += r->ncmds; }
#else
    if (r->nreloc) GBI_SEGMENT(RP_SEG, PhysicalAddr(sc));
    gbi((uint32_t)G_DL << 24, PhysicalAddr(r->gdl));
#endif

    // the state the block left
    uint32_t e, p3, v3; hle_vrdp_counters(&e, &p3, &v3);
    uint32_t extra[6]; memcpy(extra, r->exit, sizeof(extra));
    hle_vrdp_counters_set(e + extra[0], p3 + extra[1], v3 + extra[2]);
    uint32_t len = sizeof(extra);
    len += rp_exit_io(r->exit + len, true, r->ovl_mask);
    hle_t3d_zvalid_min((int)extra[3]);
    for (int k = 0; k < 16; k++) if (extra[4] & (1u << k)) len += hle_t3d_slot_io(r->exit + len, k, true);
    for (int k = 0; k < 32; k++) if (extra[5] & (1u << k)) len += hle_vrsp_pstack_io(r->exit + len, k, true);
    if (r->rdp_abs) hle_vrsp_rdp_set(r->rdp_cur, r->rdp_sentinel, r->rdp_dp, r->rdp_syncfull);
    else hle_vrsp_rdp_advance(r->rdp_rel);
    return true;
}

/**
 * A CALL into `target` from pointer stack slot `slot`. Returns true if the block
 * was replayed (the caller skips it), false to run it (it may be recorded).
 */
bool hle_replay_call(uint32_t target, int slot)
{
#ifdef HLE_NO_REPLAY    // test aid: every block interpreted
    return false;
#endif
    if (rp.on) return false;                        // nested: part of the block being recorded
    rp_block_t *b = rp_find(target, false);
    if (!b) return false;
    hle_t3d_flush();
#ifdef HLE_REPLAY_FLUSH_ONLY    // test aid: the entry flush alone
    return false;
#endif
    if (rp_ndead) rp_collect();

    // the biggest recording must fit without splitting the task while checking
    uint32_t need_cmds = 0, need_sc = 0;
    for (int i = 0; i < RP_RECS; i++) if (b->rec[i]) {
        if (b->rec[i]->ncmds > need_cmds) need_cmds = b->rec[i]->ncmds;
        if (b->rec[i]->scratch_bytes > need_sc) need_sc = b->rec[i]->scratch_bytes;
    }
#ifdef HLE_REPLAY_NO_RESERVE  // test aid
    need_cmds = 0;
#endif
#if defined(HLE_TEST_REPLAY_CHECK) || defined(HLE_REPLAY_INLINE)
    if (need_cmds) gbi_reserve(need_cmds + 8, need_sc + 64);
#else
    if (need_cmds) gbi_reserve(8, need_sc + 64);     // a replay is a segment and a call
#endif

    uint32_t mask = hle_vrsp_loaded_mask();
    static uint8_t sig[RP_SIG_MAX];
    uint32_t sig_len = 0, sig_mask = ~0u;
    uint32_t gen = hle_vc_gen();
    for (int i = 0; i < RP_RECS; i++) {
        rp_rec_t *r = b->rec[i];
        if (!r || r->vc_gen != gen || (r->ovl_mask & ~mask)) continue;
        if (r->ovl_mask != sig_mask) { sig_len = rp_signature(sig, r->ovl_mask); sig_mask = r->ovl_mask; }
        if (sig_len != r->sig_len || memcmp(sig, r->sig, sig_len)) continue;
        if (!hle_vrsp_rdp_synced() || (!r->rdp_abs && !hle_vrsp_rdp_room(r->rdp_rel))) continue;
        r->lru = ++rp_lru;
#ifdef HLE_TEST_REPLAY_CHECK
        rp_begin(b, slot);
        rp.check = r;
        return false;
#else
#ifdef HLE_TEST_PERF
        hle_rp_stats[0]++;
#endif
        return rp_replay(r);
#endif
    }
#if defined(HLE_TEST_PERF) || defined(HLE_TEST_REPLAY_CHECK)
    hle_rp_stats[3]++;
#endif
    if (b->skip) { b->skip--; return false; }
#ifdef HLE_TEST_REPLAY_CHECK    // why the newest recording didn't match: the first differing byte of the signature
    {
        static int logged;
        rp_rec_t *r = NULL;
        for (int i = 0; i < RP_RECS; i++) if (b->rec[i] && (!r || b->rec[i]->lru > r->lru)) r = b->rec[i];
        if (r && logged < 40) {
            uint32_t n = rp_signature(sig, r->ovl_mask);
            uint32_t d = 0; while (d < n && d < r->sig_len && sig[d] == r->sig[d]) d++;
            if (d < n || n != r->sig_len) {
                logged++;
                debugf("[replay] block %08lx: signature differs at byte %lu of %lu (gen %lu/%lu mask %lx/%lx): %02x -> %02x\n", b->addr, d, n,
                       r->vc_gen, gen, r->ovl_mask, mask, d < r->sig_len ? r->sig[d] : 0, d < n ? sig[d] : 0);
            } else if (logged < 40) {
                logged++;
                uint32_t c, st, dp; uint8_t sf; hle_vrsp_rdp_get(&c, &st, &dp, &sf);
                debugf("[replay] block %08lx: same signature, gen %lu/%lu mask %lx/%lx synced %d abs %d rel %lu cur %08lx sentinel %08lx\n", b->addr, r->vc_gen, gen,
                       r->ovl_mask, mask, hle_vrsp_rdp_synced(), r->rdp_abs, r->rdp_rel, c, st);
            }
        }
    }
#endif
    rp_begin(b, slot);
    return false;
}

/** A RET from pointer stack slot `slot`. */
void hle_replay_ret(int slot)
{
    if (rp.on && slot == rp.slot) rp_end();
}

/** The queue switched to highpri, or anything else a recording can't follow. */
void hle_replay_abort(void) { if (rp.on) rp.bad = true; }
