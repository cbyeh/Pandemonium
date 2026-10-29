/**
 * @file rspq.c
 * @author Dennis Heinze <dennisjp.heinze@gmail.com>
 * @author Giovanni Bajo <giovannibajo@gmail.com>
 * @brief RSP Command queue
 * @ingroup rsp
 *
 * # RSP Queue: implementation
 * 
 * This documentation block describes the internal workings of the RSP Queue.
 * This is useful to understand the implementation. For description of the
 * API of the RSP queue, see rspq.h
 *
 * ## Architecture
 * 
 * The RSP queue can be thought in abstract as a single contiguous memory buffer
 * that contains RSP commands. The CPU is the writing part, which appends command
 * to the buffer. The RSP is the reading part, which reads commands and execute
 * them. Both work at the same time on the same buffer, so careful engineering
 * is required to make sure that they do not interfere with each other.
 * 
 * The complexity of this library is trying to achieve this design without any
 * explicit synchronization primitive. The basic design constraint is that,
 * in the standard code path, the CPU should be able to just append a new command
 * in the buffer without talking to the RSP, and the RSP should be able to just
 * read a new command from the buffer without talking to the CPU. Obviously
 * there are side cases where the synchronization is required (eg: if the RSP
 * catches up with the CPU, or if the CPU fins that the buffer is full), but
 * these cases should in general be rare.
 * 
 * To achieve a fully lockless approach, there are specific rules that the CPU
 * has to follow while writing to make sure that the RSP does not get confused
 * and execute invalid or partially-written commands. On the other hand, the RSP
 * must be careful in discerning between a fully-written command and a
 * partially-written command, and at the same time not waste memory bandwidth
 * to continuously "poll" the buffer when it has caught up with the CPU.
 * 
 * The RSP uses the following algorithm to parse the buffer contents. Assume for
 * now that the buffer is linear and unlimited in size.
 * 
 * 1. The RSP fetches a "portion" of the buffer from RDRAM to DMEM. The size
 *    of the portion is RSPQ_DMEM_BUFFER_SIZE. It also resets its internal
 *    read pointer to the start of the DMEM buffer.
 * 2. The RSP reads the first byte pointed by the internal read pointer. The
 *    first byte is the command ID. It splits it into overlay ID (4 bits) and
 *    command index (4 bits).
 * 3. If the command is 0x00 (overlay 0, index 0), it means that the RSP has
 *    caught up with the CPU and there are no more pending commands.
 *    
 *     * The RSP checks whether the signal SIG_MORE was set by the CPU. This
 *       signal is set any time the CPU writes a new command in the queue.
 *       If the signal is set, it means that the CPU has continued writing but
 *       the RSP has probably fetched the buffer before those commands were
 *       written. The RSP goes back to step 1 (refetch the buffer, from the
 *       current position).
 *     * If SIG_MORE is not set, the RSP has really caught up the CPU, and no
 *       more commands are available in the queue. The RSP goes to sleep via
 *       the BREAK opcode, and waits for the CPU to wake it up when more
 *       commands are available.
 *     * After the CPU has woken the RSP, it goes back to step 1.
 * 
 * 4. If the overlay ID refers to an overlay which is not the currently loaded
 *    one, the RSP loads the new overlay into IMEM/DMEM. Before doing so, it
 *    also saves the current overlay's state back into RDRAM (this is a portion
 *    of DMEM specified by the overlay itself as "state", that is preserved
 *    across overlay switching).
 * 5. The RSP uses the command index to fetch the "command descriptor", a small
 *    structure that contains a pointer to the function in IMEM that executes
 *    the command, and the size of the command in word.
 * 6. If the command overflows the internal buffer (that is, it is longer than
 *    the number of bytes left in the buffer), it means that we need to
 *    refetch a subsequent portion of the buffer to see the whole command. Go back
 *    to step 1.
 * 7. The RSP jumps to the function that executes the command. After the command
 *    is finished, the function is expected to jump back to the main loop, going
 *    to step 2.
 *     
 * Given the above algorithm, it is easy to understand how the CPU must behave
 * when filling the buffer:
 * 
 *   * The buffer must be initialized with 0x00. This makes sure that unwritten
 *     portions of the buffers are seen as "special command 0x00" by the RSP.
 *   * The CPU must take special care not to write the command ID before the
 *     full command is written. For instance let's say a command is made by
 *     two words: 0xAB000001 0xFFFF8000 (overlay 0xA, command index 0xB,
 *     length 2). If the CPU writes the two words in the standard order,
 *     there might be a race where the RSP reads the memory via DMA when
 *     only the first word has been written, and thus see 0xAB000001 0x00000000,
 *     executing the command with a wrong second word. So the CPU has to
 *     write the first word as last (or at least its first byte must be
 *     written last).
 *   * It is important that the C compiler does not reorder writes. In general,
 *     compilers are allowed to change the order in which writes are performed
 *     in a buffer. For instance, if the code writes to buf[0], buf[1], buf[2],
 *     the compiler might decide to generate code that writes buf[2] first,
 *     for optimization reasons. It is possible to fix it using the #MEMORY_BARRIER
 *     macro, or the volatile qualifier (which guarantees a fixed order of
 *     accesses between volatile pointers, though non-volatile accesses can
 *     be reordered freely also across volatile ones).
 *
 * ## Internal commands
 *
 * To manage the queue and implement all the various features, rspq reserves
 * for itself the overlay ID 0x0 to implement internal commands. You can
 * look at the list of commands and their description below. All command IDs
 * are defined with `RSPQ_CMD_*` macros.
 * 
 * ## Buffer swapping
 * 
 * Internally, double buffering is used to implement the queue. The size of
 * each of the buffers is RSPQ_DRAM_LOWPRI_BUFFER_SIZE. When a buffer is full,
 * the low-priority queue engine stages the address of the other buffer with a
 * #RSPQ_CMD_WRITE_WORD, then executes #RSPQ_CMD_SWAP_BUFFERS. The latter sets
 * the buffer-done signal and jumps to the other buffer. The signal tells
 * the CPU when a buffer becomes free for reuse.
 * 
 * This logic is implemented in #rspq_next_buffer.
 *
 * ## Blocks
 * 
 * Blocks are implemented by redirecting #rspq_write to a different memory buffer,
 * allocated for the block. The starting size for this buffer is
 * RSPQ_BLOCK_MIN_SIZE. If the buffer becomes full, a new buffer is allocated
 * with double the size (to achieve exponential growth), and it is linked
 * to the previous buffer via a #RSPQ_CMD_JUMP. So a block can end up being
 * defined by multiple memory buffers linked via jumps.
 * 
 * Calling a block requires some work because of the nesting calls we want
 * to support. To make the RSP ucode as short as possible, the two internal
 * command dedicated to block calls (#RSPQ_CMD_CALL and #RSPQ_CMD_RET) do not
 * manage a call stack by themselves, but only allow to save/restore the
 * current queue position from a "save slot", whose index must be provided
 * by the CPU. 
 * 
 * Thus, the CPU has to make sure that each CALL opcode saves the
 * position into a save slot which will not be overwritten by nested block
 * calls. To do this, it calculates the "nesting level" of a block at
 * block creation time: the nesting level of a block is defined by the smallest
 * number greater than the nesting levels of all blocks that are called within
 * the block itself. So for instance if a block calls another block whose
 * nesting level is 5, it will get assigned a level of 6. The nesting level
 * is then used as call slot in both all future calls to the block, and by
 * the RSPQ_CMD_RET command placed at the end of the block itself.
 * 
 * ## Queues
 * 
 * Queues are mutable command chains built in RDRAM, similar to blocks but
 * intended to be reused and cleared. Internally, a queue consists of one or
 * more chunks linked by #RSPQ_CMD_JUMP guards at the end of each chunk, with
 * the last chunk ending in #RSPQ_CMD_RET. Chunk growth reuses the same doubling
 * strategy used by blocks.
 * 
 * Recording a queue redirects #rspq_write into the queue buffer, just like
 * blocks. The main difference is that #rspq_queue_run enqueues a CALL in the
 * lowpri queue and advances an internal run_start pointer. The current write
 * pointer remains in the queue, so commands appended during execution will be
 * picked up on the next #rspq_queue_run call.
 * 
 * ## Highpri queue
 * 
 * The high priority queue is implemented as an alternative couple of buffers,
 * that replace the standard buffers when the high priority mode is activated.
 * 
 * When #rspq_highpri_begin is called, the CPU notifies the RSP that it must
 * switch to the highpri queues by setting signal SP_STATUS_SIG_HIGHPRI_REQUESTED.
 * The RSP checks for that signal between each command, and when it sees it, it
 * internally calls #RSPQ_CMD_SWAP_BUFFERS. This command loads the highpri queue
 * pointer from a special call slot, saves the current lowpri queue position
 * in another special save slot, and finally clear SP_STATUS_SIG_HIGHPRI_REQUESTED
 * and set SP_STATUS_SIG_HIGHPRI_RUNNING instead.
 * 
 * When the #rspq_highpri_end is called, the opposite is done. The CPU writes
 * in the queue a #RSPQ_CMD_SWAP_BUFFERS that saves the current highpri pointer
 * into its call slot, recover the previous lowpri position, and turns off
 * SP_STATUS_SIG_HIGHPRI_RUNNING.
 * 
 * Some careful tricks are necessary to allow multiple highpri queues to be
 * pending, see #rspq_highpri_begin for details.
 * 
 * ## rdpq integrations
 * 
 * There are a few places where the rsqp code is hooked with rdpq to provide
 * for coherent usage of the two peripherals. In particular:
 * 
 *  * #rspq_wait automatically calls #rdpq_fence. This means that
 *    it will also wait for RDP to finish executing all commands, which is
 *    actually expected for its intended usage of "full sync for debugging
 *    purposes".
 *  * All rsqp block creation functions call into hooks in rdpq. This is
 *    necessary because blocks are specially handled by rdpq via static
 *    buffer, to make sure RDP commands in the block don't passthrough
 *    via RSP, but are directly DMA from RDRAM into RDP. Moreover,
 *    See rdpq.c documentation for more details. 
 *  * In specific places, we call into the rdpq debugging module to help
 *    tracing the RDP commands. For instance, when switching RDP RDRAM
 *    buffers, RSP will generate an interrupt to inform the debugging
 *    code that it needs to finish dumping the previous RDP buffer.
 * 
 */

#include "rsp.h"
#include "rspq.h"
#include "rspq_internal.h"
#include "rspq_constants.h"
#include "rspq_profile.h"
#include "rdp.h"
#include "rdpq_constants.h"
#include "rdpq/rdpq_internal.h"
#include "rdpq/rdpq_debug_internal.h"
#include "interrupt.h"
#include "utils.h"
#include "n64sys.h"
#include "debug.h"
#include "accounting_internal.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <malloc.h>
#include "hle.h"

// HLE build: this is libdragon's rspq.c with the RSP replaced by a CPU interpreter
// (vrsp_run, see bottom of file). SP_STATUS here is a virtual register: the real one
// is owned by the HLE emulator, which reuses SIG2 as its "task done" flag.
static volatile uint32_t vsp_status = SP_STATUS_HALTED;
static void vsp_write(uint32_t w);
static void vrsp_run(void);
static void vrsp_save_overlay_state(void);
static vrsp_handler_t vrsp_find_handler(rsp_ucode_t *ucode);
static vrsp_handler_t vrsp_handlers[16];   // C implementation of each overlay slot
static int vrsp_cur_ovl;                   // overlay currently "loaded" in virtual DMEM (0 = none)
static uint32_t vrsp_ptr;                  // RDRAM address of the next command to run
#define PSTACK(i) (((uint32_t*)rspq_data.rspq_pointer_stack)[i])  // includes the lowpri/highpri slots

/// @cond
// Make sure that RSPQ_CMD_WRITE_STATUS and RSPQ_CMD_TEST_WRITE_STATUS have
// an even ID number. This is a small trick used to save one opcode in
// rsp_queue.S (see cmd_write_status there for an explanation).
_Static_assert((RSPQ_CMD_WRITE_STATUS & 1) == 0);
_Static_assert((RSPQ_CMD_TEST_WRITE_STATUS & 1) == 0);

// Check that the DMEM buffer is sized at least for the largest command
// that we can handle, plus some extra space that's required because
// the RSP code won't run a command that ends exactly at the end of
// the buffer (see rsp_queue.inc).
_Static_assert(RSPQ_DMEM_BUFFER_SIZE >= (RSPQ_MAX_COMMAND_SIZE + 2) * 4);

// Check that the maximum command size is actually supported by the 
// internal command descriptor format.
_Static_assert(RSPQ_MAX_COMMAND_SIZE * 4 <= RSPQ_DESCRIPTOR_MAX_SIZE);
/// @endcond

/** @brief Smaller version of rspq_write that writes to an arbitrary pointer */
#define rspq_append1(ptr, cmd, arg1) ({ \
    ((volatile uint32_t*)(ptr))[0] = ((cmd)<<24) | (arg1); \
    ptr += 1; \
})

/** @brief Smaller version of rspq_write that writes to an arbitrary pointer */
#define rspq_append2(ptr, cmd, arg1, arg2) ({ \
    ((volatile uint32_t*)(ptr))[1] = (arg2); \
    ((volatile uint32_t*)(ptr))[0] = ((cmd)<<24) | (arg1); \
    ptr += 2; \
})

/** @brief Smaller version of rspq_write that writes to an arbitrary pointer */
#define rspq_append3(ptr, cmd, arg1, arg2, arg3) ({ \
    ((volatile uint32_t*)(ptr))[1] = (arg2); \
    ((volatile uint32_t*)(ptr))[2] = (arg3); \
    ((volatile uint32_t*)(ptr))[0] = ((cmd)<<24) | (arg1); \
    ptr += 3; \
})

/** @brief Number of words reserved for the lowpri buffer handoff sequence. */
#define RSPQ_LOWPRI_HANDOFF_WORDS 5

/** @brief Block-call slot temporarily reused by the lowpri buffer handoff. */
#define RSPQ_LOWPRI_HANDOFF_SLOT 0

static void rspq_crash_handler(rsp_snapshot_t *state);
static void rspq_assert_handler(rsp_snapshot_t *state, uint16_t assert_code);

/** The RSPQ ucode */
DEFINE_RSP_UCODE(rsp_queue, 
    .crash_handler = rspq_crash_handler,
    .assert_handler = rspq_assert_handler);

/** 
 * @brief The header of the overlay in DMEM.
 * 
 * This structure is placed at the start of the overlay in DMEM, via the
 * RSPQ_OverlayHeader macros (defined in rsp_queue.inc).
 */
typedef struct __attribute__((packed)) rspq_overlay_header_t {
    uint16_t state_start;       ///< Start of the portion of DMEM used as "state"
    uint16_t state_size;        ///< Size of the portion of DMEM used as "state" (minus 1)
    uint32_t state_rdram;       ///< RDRAM address of the portion of DMEM used as "state"
    uint16_t command_base;      ///< Primary overlay ID used for this overlay
    uint16_t overlay_id;        ///< Overlay ID (multiplied by 4)
    uint32_t text_rdram;        ///< RDRAM address of the overlay's text section
    uint16_t text_size;         ///< Size of the overlay's text section (minus 1)
    uint16_t text_start;        ///< Offset of the overlay's text section in DMEM
    uint32_t extraseg_rdram;    ///< RDRAM address of extra segment
    uint16_t extraseg_size;     ///< Size of extra segment (minus 1)
    uint16_t extraseg_start;    ///< Offset of extra segment in DMEM
    #if RSPQ_PROFILE
    uint16_t profile_slot_dmem; ///< Start of the profile slots in DMEM
    #endif
    uint16_t commands[];
} rspq_overlay_header_t;

///@cond
_Static_assert(sizeof(rspq_overlay_header_t) == RSPQ_OVERLAY_HEADER_SIZE);
///@endcond

/** @brief RSPQ overlays */
rsp_ucode_t *rspq_overlay_ucodes[RSPQ_MAX_OVERLAYS];

/**
 * @brief RSP queue building context
 * 
 * This structure contains the state of a RSP queue as it is built by the CPU.
 * It is instantiated two times: one for the lwopri queue, and one for the
 * highpri queue. It contains the two buffers used in the double buffering
 * scheme, and some metadata about the queue.
 * 
 * The current write pointer is stored in the "cur" field. The "sentinel" field
 * contains the pointer to the last byte at which a new command can start,
 * before overflowing the buffer (given #RSPQ_MAX_COMMAND_SIZE). This is used
 * for efficiently check when it is time to switch to the other buffer: basically,
 * it is sufficient to check whether "cur > sentinel".
 * 
 * The current queue is stored in 3 global pointers: #rspq_ctx, #rspq_cur_pointer
 * and #rspq_cur_sentinel. #rspq_cur_pointer and #rspq_cur_sentinel are 
 * external copies of the "cur" and "sentinel" pointer of the
 * current context, but they are kept as separate global variables for
 * maximum performance of the hottest code path: #rspq_write. In fact, it is
 * much faster to access a global 32-bit pointer (via gp-relative offset) than
 * dereferencing a member of a global structure pointer.
 * 
 * rspq_switch_context is called to switch between lowpri and highpri,
 * updating the three global pointers.
 * 
 * When building a block, #rspq_ctx is set to NULL, while the other two
 * pointers point inside the block memory.
 */
typedef struct {
    void *buffers[2];                   ///< The two buffers used to build the RSP queue
    int buf_size;                       ///< Size of each buffer in 32-bit words
    int buf_idx;                        ///< Index of the buffer currently being written to.
    uint32_t sp_status_bufdone;         ///< SP status bit to signal that one buffer has been run by RSP
    uint32_t sp_wstatus_set_bufdone;    ///< SP mask to set the bufdone bit
    uint32_t sp_wstatus_clear_bufdone;  ///< SP mask to clear the bufdone bit
    volatile uint32_t *cur;             ///< Current write pointer within the active buffer
    volatile uint32_t *sentinel;        ///< Current write sentinel within the active buffer
} rspq_ctx_t;

/**
 * @brief Command chain used while a block or a queue is being recorded.
 *
 * A command chain is a linked list of chunks of commands. Each chunk is a
 * contiguous block of commands that is allocated from the heap.
 *
 * When RSP executes a command chain, it will start from the first chunk, and
 * follow the JUMP commands to the next chunk until it reaches the last chunk.
 * The last chunk is terminated with a RET command.
 *
 * To simplify traversal of the chain by the CPU, the final JUMP/RET command
 * is also always duplicated as last word in each chunk.
 */
typedef struct rspq_cmd_chain_s {
    uint32_t *first_chunk;          ///< First chunk of commands
    uint32_t *cur_chunk;            ///< Current chunk being written
    int cur_chunk_size;             ///< Size of the current chunk (in 32-bit words)
    volatile uint32_t *cur;         ///< Current write pointer within the current chunk
    volatile uint32_t *sentinel;    ///< Current write sentinel within the current chunk
} rspq_cmd_chain_t;

/**
 * @brief A rspq queue: mutable buffered sequence of commands
 */
typedef struct rspq_queue_s {
    rspq_cmd_chain_t chain;            ///< Command chain for this queue
    uint32_t nesting_level;            ///< Nesting level of the queue
    volatile uint32_t *run_start;      ///< Start address for the next run
    rdpq_tracking_t rdpq_tracking;     ///< Tracking state of the queue
    uint32_t cmds[];                   ///< First chunk contents
} rspq_queue_t;

static rspq_ctx_t lowpri;               ///< Lowpri queue context
static rspq_ctx_t highpri;              ///< Highpri queue context
static int highpri_nesting;             ///< Nesting level of #rspq_highpri_begin

rspq_ctx_t *rspq_ctx;                   ///< Current context
volatile uint32_t *rspq_cur_pointer;    ///< Copy of the current write pointer (see #rspq_ctx_t)
volatile uint32_t *rspq_cur_sentinel;   ///< Copy of the current write sentinel (see #rspq_ctx_t)

/** @brief Buffers that hold outgoing RDP commands (generated via RSP). */
void *rspq_rdp_dynamic_buffers[2];

/** @brief RSP queue data in DMEM (the virtual DMEM starts with it, like the real one). */
uint8_t __vrsp_dmem[4096] __attribute__((aligned(16)));
#define rspq_data (*(rsp_queue_t*)__vrsp_dmem)

/** @brief True if the queue system has been initialized. */
static bool rspq_initialized = 0;

/** @brief Pointer to the current block being built, or NULL. */
rspq_block_t *rspq_block;
/** @brief Command chain used while a block is being recorded. */
static rspq_cmd_chain_t rspq_block_chain;
/** @brief Pointer to the current queue being recorded, or NULL. */
static rspq_queue_t *rspq_queue_recording;

/** @brief ID that will be used for the next syncpoint that will be created. */
static int rspq_syncpoints_genid;
/** @brief ID of the last syncpoint reached by RSP (plus padding). */
volatile int __rspq_syncpoints_done[4]  __attribute__((aligned(16)));

/** @brief True if the RSP queue engine is running in the RSP. */
static bool rspq_is_running;

/** @brief Dummy state used for overlay 0 */
static uint64_t dummy_overlay_state[2] __attribute__((aligned(16)));

/** @brief Deferred calls: head of list */
static rspq_deferred_call_t *defcalls_head;
/** @brief Deferred calls: tail of list */
static rspq_deferred_call_t *defcalls_tail;
/** @brief Deferred calls: count */
static uint32_t defcalls_count;
/** @brief Deferred calls: maximum count */
static uint32_t defcalls_run_threshold;

static void rspq_flush_internal(void);
static volatile uint32_t* rspq_switch_buffer(uint32_t *new, int size, bool clear);

/** @brief Reset a command chain to the initial state. */
static void rspq_chain_reset(rspq_cmd_chain_t *ch, uint32_t *first, int size)
{
    ch->first_chunk = first;
    ch->cur_chunk = first;
    ch->cur_chunk_size = size;
    ch->cur = ch->cur_chunk;
    ch->sentinel = ch->cur_chunk + size - (RSPQ_MAX_SHORT_COMMAND_SIZE + 2);
}

/* @brief Initialize a command chain */
static void rspq_chain_init(rspq_cmd_chain_t *ch, uint32_t *first, int size)
{
    rspq_chain_reset(ch, first, size);

    // Guard word at the end of the chunk (used for traversal on destroy).
    volatile uint32_t *guard = first + size - 1;
    rspq_append1(guard, RSPQ_CMD_RET, 0);
}

/* @brief Create/advance to the next chunk in a command chain.
   Returns the pointer to the next chunk. */
static uint32_t* rspq_chain_next(rspq_cmd_chain_t *ch)
{
    volatile uint32_t *prev_guard = ch->cur_chunk + ch->cur_chunk_size - 1;
    uint32_t cmd = *prev_guard;
    uint32_t *next = NULL;
    int next_size = ch->cur_chunk_size;

    if (cmd >> 24 == RSPQ_CMD_JUMP) {
        next = VirtualUncachedAddr(cmd & 0xFFFFFF);
        if (next_size < RSPQ_BLOCK_MAX_SIZE) next_size *= 2;
    } else if (cmd >> 24 == RSPQ_CMD_RET) {
        if (next_size < RSPQ_BLOCK_MAX_SIZE) next_size *= 2;
        next = malloc_uncached(next_size * sizeof(uint32_t));
        assertf(next, "Out of memory");

        volatile uint32_t *guard = next + next_size - 1;
        rspq_append1(guard, RSPQ_CMD_RET, 0);

        rspq_append1(prev_guard, RSPQ_CMD_JUMP, PhysicalAddr(next));
    } else {
        assertf(0, "invalid terminator command in chain: %08lx\n", cmd);
    }

    // Write the JUMP command for the RSP at the current write position
    rspq_append1(rspq_cur_pointer, RSPQ_CMD_JUMP, PhysicalAddr(next));

    ch->cur_chunk = next;
    ch->cur_chunk_size = next_size;

    rspq_switch_buffer(next, next_size, false);
    ch->cur = rspq_cur_pointer;
    ch->sentinel = rspq_cur_sentinel;

    return next;
}

/** @brief Free a command chain. */
static void rspq_chain_free(uint32_t *first_chunk, int size)
{
    uint32_t *ptr = first_chunk;
    while (1) {
        uint32_t cmd = ptr[size-1];

        // Free all the chunks except the first one (builtin in
        // the containing structure)
        if (ptr != first_chunk)
            free_uncached(ptr);

        // If the guard command is a jump, there is a next chunk
        if (cmd >> 24 == RSPQ_CMD_JUMP) {
            ptr = UncachedAddr(0x80000000 | (cmd & 0xFFFFFF));
            if (size < RSPQ_BLOCK_MAX_SIZE) size *= 2;
            continue;
        }
        // If the guard command is a return, we are done
        if (cmd >> 24 == RSPQ_CMD_RET) {
            return;
        }
        assertf(0, "invalid terminator command in chain: %08lx\n", cmd);
    }
}

/** @brief RSP interrupt handler, used for syncpoints. */
static void rspq_sp_interrupt(void)
{
    uint32_t status = vsp_status;
    uint32_t wstatus = 0;

    // Check if a syncpoint was reached by RSP. If so, increment the
    // syncpoint done ID and clear the signal.
    if (status & SP_STATUS_SIG_SYNCPOINT) {
        wstatus |= SP_WSTATUS_CLEAR_SIG_SYNCPOINT;
        ++__rspq_syncpoints_done[0];
        // writeback to memory; this is required for RDPQCmd_SyncFull to fetch the correct value 
        data_cache_hit_writeback(&__rspq_syncpoints_done[0], 4);
    }
    if (status & SP_STATUS_SIG0) {
        wstatus |= SP_WSTATUS_CLEAR_SIG0;
        if (rdpq_trace_fetch) rdpq_trace_fetch(true);
    }

    MEMORY_BARRIER();

    if (wstatus)
        vsp_write(wstatus);
}

/** @brief Extract the current overlay index and name from the RSP queue state */
static void rspq_get_current_ovl(rspq_overlay_header_t *ovl_header, uint8_t *ovl_id, const char **ovl_name)
{
    *ovl_id = ovl_header->overlay_id >> 2;
    if (*ovl_id == 0) {
        *ovl_name = "builtin";
    } else if (*ovl_id < RSPQ_MAX_OVERLAYS && rspq_overlay_ucodes[*ovl_id]) {
        *ovl_name = rspq_overlay_ucodes[*ovl_id]->name;
    } else
        *ovl_name = "?";
}

/** @brief RSPQ crash handler. This shows RSPQ-specific info the in RSP crash screen. */
static void rspq_crash_handler(rsp_snapshot_t *state)
{
    uint32_t rsp_queue_data_size = rsp_queue_data_end - rsp_queue_data_start;
    rsp_queue_t *rspq = (rsp_queue_t*)(state->dmem);
    rspq_overlay_header_t *ovl_header = (rspq_overlay_header_t*)(state->dmem + rsp_queue_data_size);
    uint32_t cur = rspq->rspq_dram_addr + state->gpr[28];

    const char *ovl_name; uint8_t ovl_id;
    rspq_get_current_ovl(ovl_header, &ovl_id, &ovl_name);

    printf("RSPQ: Normal  DRAM address: %08lx\n", rspq->rspq_dram_lowpri_addr);
    printf("RSPQ: Highpri DRAM address: %08lx\n", rspq->rspq_dram_highpri_addr);
    printf("RSPQ: Current DRAM address: %08lx + GP=%lx = %08lx\n", 
        rspq->rspq_dram_addr, state->gpr[28], cur);
    printf("RSPQ: RDP     DRAM address: %08lx\n", rspq->rspq_rdp_buffers[1]);
    printf("RSPQ: Current Overlay: %s (%x)\n", ovl_name, ovl_id);

    // Dump the command queue in DMEM. In debug mode, there is a marker to check
    // if we know the correct address. TODO: find a way to expose the symbols
    // from rsp_queue.inc.
    debugf("RSPQ: Command queue:\n");
    for (int j=0;j<4;j++) {        
        for (int i=0;i<16;i++)
            debugf("%08lx%c", rspq->cmds[i+j*16], state->gpr[28] == (j*16+i)*4 ? '*' : ' ');
        debugf("\n");
    }

    // Dump the command queue in RDRAM (both data before and after the current pointer).
    debugf("RSPQ: RDRAM Command queue: %s\n", (cur&3) ? "MISALIGNED" : "");
    uint32_t *q = (uint32_t*)(0xA0000000 | (cur & 0xFFFFFC));
    for (int j=0;j<4;j++) {        
        for (int i=0;i<16;i++)
            debugf("%08lx%c", q[i+j*16-32], i+j*16-32==0 ? '*' : ' ');
        debugf("\n");
    }

    debugf("RSPQ: RDP Command queue: %s\n", (cur&7) ? "MISALIGNED" : "");
    q = (uint32_t*)(0xA0000000 | (state->cop0[10] & 0xFFFFF8));
    for (int j=0;j<4;j++) {        
        for (int i=0;i<16;i+=2) {
            debugf("%08lx", q[i+0+j*16-32]);
            debugf("%08lx%c", q[i+1+j*16-32], i+j*16-32==0 ? '*' : ' ');
        }
        debugf("\n");
    }
}

/** @brief Special RSP assert handler for ASSERT_INVALID_COMMAND */
static void rspq_assert_invalid_command(rsp_snapshot_t *state)
{
    rspq_overlay_header_t *ovl_header = (rspq_overlay_header_t*)(state->dmem + (rsp_queue_data_end - rsp_queue_data_start));
    const char *ovl_name; uint8_t ovl_id;
    rspq_get_current_ovl(ovl_header, &ovl_id, &ovl_name);

    uint32_t cur = state->gpr[28]/4;
    printf("Invalid command\nCommand %02x not found in overlay %s (0x%01x)\n", state->dmem[cur], ovl_name, ovl_id);
}

/** @brief Special RSP assert handler for ASSERT_INVALID_OVERLAY */
static void rspq_assert_invalid_overlay(rsp_snapshot_t *state)
{
    printf("Invalid overlay\nOverlay 0x%01lx not registered\n", state->gpr[15]);
}

/** @brief RSP assert handler for rspq */
static void rspq_assert_handler(rsp_snapshot_t *state, uint16_t assert_code)
{
    switch (assert_code) {
        case ASSERT_INVALID_OVERLAY:
            rspq_assert_invalid_overlay(state);
            break;
        case ASSERT_INVALID_COMMAND:
            rspq_assert_invalid_command(state);
            break;
        default: {
            // Check if there is an assert handler for the current overlay.
            // If it exists, forward request to it.
            // Be defensive against DMEM corruptions.
            rspq_overlay_header_t *ovl_header = (rspq_overlay_header_t*)(state->dmem + (rsp_queue_data_end - rsp_queue_data_start));
            uint8_t ovl_id = ovl_header->overlay_id >> 2;
            if (ovl_id < RSPQ_MAX_OVERLAYS &&
                rspq_overlay_ucodes[ovl_id] &&
                rspq_overlay_ucodes[ovl_id]->assert_handler)
                rspq_overlay_ucodes[ovl_id]->assert_handler(state, assert_code);
            else
                printf("\n");
            break;
        }
    }
}

/** @brief Switch current queue context (used to switch between highpri and lowpri) */
__attribute__((noinline))
static void rspq_switch_context(rspq_ctx_t *new)
{
    if (rspq_ctx) {
        // Save back the external pointers into the context structure, where
        // they belong.
        rspq_ctx->cur = rspq_cur_pointer;
        rspq_ctx->sentinel = rspq_cur_sentinel;
    }

    // Switch to the new context, and make an external copy of cur/sentinel
    // for performance reason.
    rspq_ctx = new;
    rspq_cur_pointer = rspq_ctx ? rspq_ctx->cur : NULL;
    rspq_cur_sentinel = rspq_ctx ? rspq_ctx->sentinel : NULL;
}

/** @brief Switch the current write buffer */
static volatile uint32_t* rspq_switch_buffer(uint32_t *new, int size, bool clear)
{
    volatile uint32_t* prev = rspq_cur_pointer;

    // Notice that the buffer must have been cleared before, as the
    // command queue are expected to always contain 0 on unwritten data.
    // We don't do this for performance reasons.
    int terminator_words = clear && rspq_ctx == &lowpri ?
        RSPQ_LOWPRI_HANDOFF_WORDS : 2;
    assert(size >= RSPQ_MAX_COMMAND_SIZE + terminator_words);
    if (clear) memset(new, 0, size * sizeof(uint32_t));

    // Switch to the new buffer, and calculate the new sentinel. The sentinel
    // must allow for a maximum size command (RSPQ_MAX_SHORT_COMMAND_SIZE) to
    // be written, plus either the lowpri handoff sequence or the two-word
    // reservation used by highpri, blocks, and recorded queues.
    rspq_cur_pointer = new;
    rspq_cur_sentinel = new + size -
        (RSPQ_MAX_SHORT_COMMAND_SIZE + terminator_words);

    // Return a pointer to the previous buffer
    return prev;
}

/** @brief Start the RSP queue engine in the RSP */
static void rspq_start(void)
{
    if (rspq_is_running)
        return;

    // Set initial value of all signals (virtual RSP: nothing to load).
    vsp_write(SP_WSTATUS_CLEAR_SIG0 |
              SP_WSTATUS_CLEAR_SIG1 |
              SP_WSTATUS_CLEAR_SIG_HIGHPRI_RUNNING |
              SP_WSTATUS_CLEAR_SIG_SYNCPOINT |
              SP_WSTATUS_SET_SIG_BUFDONE_LOW |
              SP_WSTATUS_SET_SIG_BUFDONE_HIGH |
              SP_WSTATUS_CLEAR_SIG_HIGHPRI_REQUESTED |
              SP_WSTATUS_CLEAR_SIG_MORE);
    rspq_is_running = 1;
}

/** @brief Initialize a rspq_ctx_t structure */
static void rspq_init_context(rspq_ctx_t *ctx, int buf_size)
{
    memset(ctx, 0, sizeof(rspq_ctx_t));
    ctx->buffers[0] = malloc_uncached(buf_size * sizeof(uint32_t));
    ctx->buffers[1] = malloc_uncached(buf_size * sizeof(uint32_t));
    assertf(ctx->buffers[0] && ctx->buffers[1], "Out of memory");
    memset(ctx->buffers[0], 0, buf_size * sizeof(uint32_t));
    memset(ctx->buffers[1], 0, buf_size * sizeof(uint32_t));
    ctx->buf_idx = 0;
    ctx->buf_size = buf_size;
    ctx->cur = ctx->buffers[0];
    ctx->sentinel = ctx->cur + buf_size - RSPQ_MAX_COMMAND_SIZE;
}

static void rspq_close_context(rspq_ctx_t *ctx)
{
    free_uncached(ctx->buffers[1]);
    free_uncached(ctx->buffers[0]);
}

void rspq_init(void)
{
    // Do nothing if rspq_init has already been called
    if (rspq_initialized)
        return;

    rspq_ctx = NULL;
    rspq_cur_pointer = NULL;
    rspq_cur_sentinel = NULL;

    // Allocate RSPQ contexts
    rspq_init_context(&lowpri, RSPQ_DRAM_LOWPRI_BUFFER_SIZE);
    lowpri.sp_status_bufdone = SP_STATUS_SIG_BUFDONE_LOW;
    lowpri.sp_wstatus_set_bufdone = SP_WSTATUS_SET_SIG_BUFDONE_LOW;
    lowpri.sp_wstatus_clear_bufdone = SP_WSTATUS_CLEAR_SIG_BUFDONE_LOW;

    rspq_init_context(&highpri, RSPQ_DRAM_HIGHPRI_BUFFER_SIZE);
    highpri.sp_status_bufdone = SP_STATUS_SIG_BUFDONE_HIGH;
    highpri.sp_wstatus_set_bufdone = SP_WSTATUS_SET_SIG_BUFDONE_HIGH;
    highpri.sp_wstatus_clear_bufdone = SP_WSTATUS_CLEAR_SIG_BUFDONE_HIGH;

    // Start in low-priority mode
    rspq_switch_context(&lowpri);

    // Allocate the RDP dynamic buffers.
    rspq_rdp_dynamic_buffers[0] = malloc_uncached(RDPQ_DYNAMIC_BUFFER_SIZE);
    rspq_rdp_dynamic_buffers[1] = malloc_uncached(RDPQ_DYNAMIC_BUFFER_SIZE);
    assertf(rspq_rdp_dynamic_buffers[0] && rspq_rdp_dynamic_buffers[1], "Out of memory");
    
    // Verify consistency of state
    int banner_offset = offsetof(rsp_queue_t, banner);
    assertf(!memcmp(rsp_queue.data + banner_offset, "Dragon RSP Queue", 16),
        "rsp_queue_t does not seem to match DMEM; did you forget to update it?");

    // Load initial settings
    memcpy(&rspq_data, rsp_queue.data, sizeof(rsp_queue_t));
    rspq_data.rspq_dram_lowpri_addr = PhysicalAddr(lowpri.cur);
    rspq_data.rspq_dram_highpri_addr = PhysicalAddr(highpri.cur);
    rspq_data.rspq_dram_addr = rspq_data.rspq_dram_lowpri_addr;
    vrsp_ptr = rspq_data.rspq_dram_addr;
    rspq_data.rspq_rdp_buffers[0] = PhysicalAddr(rspq_rdp_dynamic_buffers[0]);
    rspq_data.rspq_rdp_buffers[1] = PhysicalAddr(rspq_rdp_dynamic_buffers[1]);
    rspq_data.rspq_rdp_current = rspq_data.rspq_rdp_buffers[0];
    rspq_data.rspq_rdp_sentinel = rspq_data.rspq_rdp_buffers[0] + RDPQ_DYNAMIC_BUFFER_SIZE;
    rspq_data.rspq_ovl_table.data_rdram[0] = PhysicalAddr(dummy_overlay_state) | (0<<24);

#if RSPQ_PROFILE
    rspq_data.rspq_profile_cur_slot = -1;
#endif
    
    // Init syncpoints
    rspq_syncpoints_genid = 0;
    __rspq_syncpoints_done[0] = 0;

    // Init blocks
    rspq_block = NULL;
    rspq_queue_recording = NULL;
    rspq_is_running = false;
    highpri_nesting = 0;

    // Syncpoint interrupts are raised synchronously by vsp_write; the RDP is
    // virtual too (see vrdp.c), so no hardware setup is needed here.
    rspq_initialized = 1;
    vrdp_init();
    vrdp_set_start(rspq_data.rspq_rdp_current);

    rspq_start();
}

/** @brief Stop the RSP queue engine in the RSP */
static void rspq_stop(void)
{
    rspq_is_running = 0;
}

void rspq_close(void)
{
    rspq_stop();
    
    rspq_initialized = 0;

    free_uncached(rspq_rdp_dynamic_buffers[0]);
    free_uncached(rspq_rdp_dynamic_buffers[1]);

    rspq_close_context(&highpri);
    rspq_close_context(&lowpri);
}

static void* overlay_get_state(rsp_ucode_t *overlay_ucode, int *state_size)
{
    uint32_t rspq_data_size = rsp_queue_data_end - rsp_queue_data_start;
    rspq_overlay_header_t *overlay_header = (rspq_overlay_header_t*)(overlay_ucode->data + rspq_data_size);

    uint32_t state_offset = (overlay_header->state_start & 0xFFF);
    assertf(state_offset >= rspq_data_size + sizeof(rspq_overlay_header_t), 
        "Saved overlay state must start after the overlay header (overlay: %s)!", overlay_ucode->name);

    void* state_ptr = overlay_ucode->data + state_offset;
    assertf(state_ptr + overlay_header->state_size + 1 <= overlay_ucode->data_end, 
    "Saved overlay state must be completely within the data segment (overlay: %s)", overlay_ucode->name);

    if (state_size)
        *state_size = overlay_header->state_size;

    return state_ptr;
}

void* rspq_overlay_get_state(rsp_ucode_t *overlay_ucode)
{
    // Get the RDRAM pointers to the overlay state
    int state_size;
    uint8_t* state_ptr = overlay_get_state(overlay_ucode, &state_size);

    (void)state_size;
    if (rspq_is_running)
    {
        // Make sure the virtual RSP is idle, then write back the state of the
        // loaded overlay (and unload it, so CPU-side edits are picked up).
        rspq_wait();
        vrsp_save_overlay_state();
    }

    return state_ptr;
}

rsp_queue_t* __rspq_get_state(void)
{
    // The virtual DMEM *is* rspq_data, nothing to read back.
    rspq_wait();
    return &rspq_data;
}

static uint32_t rspq_overlay_get_command_count(rspq_overlay_header_t *header)
{
    for (uint32_t i = 0; i < RSPQ_MAX_OVERLAY_COMMAND_COUNT; i++)
    {
        if (header->commands[i] == 0) {
            return i;
        }
    }

    assertf(0, "Overlays can only define up to %d commands!", RSPQ_MAX_OVERLAY_COMMAND_COUNT);
    return 0;
}

static uint32_t rspq_find_new_overlay_id(uint32_t slot_count)
{
    uint32_t cur_free_slots = 0;

    for (uint32_t i = 1; i <= RSPQ_MAX_OVERLAYS - slot_count; i++)
    {
        // If this slot is occupied, reset number of free slots found
        if (rspq_data.rspq_ovl_table.data_rdram[i] != 0) {
            cur_free_slots = 0;
            continue;
        }

        ++cur_free_slots;

        // If required number of slots have not been found, keep searching
        if (cur_free_slots < slot_count) {
            continue;
        }

        // We have found <slot_count> consecutive free slots
        uint32_t found_slot = i - slot_count + 1;
        return found_slot;
    }
    
    // If no free slots have been found, return zero, which means the search failed.
    return 0;
}

static void rspq_update_tables(bool is_highpri)
{
    // Issue a DMA request to update the overlay tables in DMEM.
    // Note that we don't use rsp_load_data() here and instead use the dma command,
    // so we don't need to synchronize with the RSP. All commands queued after this
    // point will be able to use the newly registered overlay.
    data_cache_hit_writeback_invalidate(&rspq_data.rspq_ovl_table, sizeof(rspq_ovl_table_t));
    if (is_highpri) rspq_highpri_begin();
    rspq_dma_to_dmem(
        offsetof(rsp_queue_t, rspq_ovl_table),
        &rspq_data.rspq_ovl_table, sizeof(rspq_ovl_table_t), false);
    if (is_highpri) rspq_highpri_end();
}

static uint32_t rspq_overlay_register_internal(rsp_ucode_t *overlay_ucode, uint32_t static_id)
{
    assertf(rspq_initialized, "rspq_overlay_register must be called after rspq_init!");
    assert(overlay_ucode);

    // The RSPQ ucode is always linked into overlays, so we need to load the overlay from an offset.
    uint32_t rspq_text_size = rsp_queue_text_end - rsp_queue_text_start;
    uint32_t rspq_data_size = rsp_queue_data_end - rsp_queue_data_start;

    assertf(memcmp(rsp_queue_text_start, overlay_ucode->code, rspq_text_size) == 0,
        "Common code of overlay %s does not match!", overlay_ucode->name);
    assertf(memcmp(rsp_queue_data_start, overlay_ucode->data, rspq_data_size) == 0,
        "Common data of overlay %s does not match!", overlay_ucode->name);

    void *overlay_code = overlay_ucode->code + rspq_text_size;
    void *overlay_data = overlay_ucode->data + rspq_data_size;
    int overlay_data_size = (void*)overlay_ucode->data_end - overlay_data;
    int overlay_code_size = (void*)overlay_ucode->code_end - overlay_code;

    // Check if the overlay has been registered already
    for (uint32_t i = 0; i < RSPQ_MAX_OVERLAYS; i++)
    {
        assertf((rspq_data.rspq_ovl_table.data_rdram[i] & 0x00FFFFFF) != PhysicalAddr(overlay_data),
            "Overlay %s is already registered!", overlay_ucode->name);
    }

    // determine number of commands and try to allocate ID(s) accordingly
    rspq_overlay_header_t *overlay_header = (rspq_overlay_header_t*)overlay_data;
    assertf((uint16_t)(overlay_header->state_size + 1) > 0, "Size of saved state must not be zero (overlay: %s)", overlay_ucode->name);
    assertf((overlay_header->state_size + 1) <= 0x1000, "Saved state is too large: %#x", overlay_header->state_size + 1);

    uint32_t command_count = rspq_overlay_get_command_count(overlay_header);
    uint32_t slot_count = DIVIDE_CEIL(command_count, 16);

    uint32_t id = static_id >> 28;
    if (id != 0) {
        for (uint32_t i = 0; i < slot_count; i++)
        {
            assertf(rspq_data.rspq_ovl_table.data_rdram[id + i] == 0,
                "Tried to register overlay %s in already occupied slot!", overlay_ucode->name);
        }
    } else {
        id = rspq_find_new_overlay_id(slot_count);
        assertf(id != 0, "Not enough consecutive free slots available for overlay %s (%ld commands)!",
            overlay_ucode->name, command_count);
    }

    // Store the address of the data segment in the overlay table, packed with the size
    for (uint32_t i = 0; i < slot_count; i++) {
        rspq_data.rspq_ovl_table.data_rdram[id + i] = 
            PhysicalAddr(overlay_data) | (((overlay_data_size - 1) >> 4) << 24);
        rspq_data.rspq_ovl_table.idmap[id + i] = id << 2;
    }

    // Fill information in the overlay header
    overlay_header->text_size = overlay_code_size - 1;
    overlay_header->text_rdram = PhysicalAddr(overlay_code);
    overlay_header->state_rdram = PhysicalAddr(overlay_ucode->data) + overlay_header->state_start;
    overlay_header->command_base = id << 5;
    overlay_header->overlay_id = id << 2;
    data_cache_hit_writeback_invalidate(overlay_header, sizeof(rspq_overlay_header_t));

    // Save the overlay pointer
    rspq_overlay_ucodes[id] = overlay_ucode;
    for (uint32_t i = 0; i < slot_count; i++)
        vrsp_handlers[id + i] = vrsp_find_handler(overlay_ucode);

    rspq_update_tables(true);

    return id << 28;
}

uint32_t rspq_overlay_register(rsp_ucode_t *overlay_ucode)
{
    return rspq_overlay_register_internal(overlay_ucode, 0);
}

void rspq_overlay_register_static(rsp_ucode_t *overlay_ucode, uint32_t overlay_id)
{
    assertf((overlay_id & 0x0FFFFFFF) == 0, 
        "the specified overlay_id should only use the top 4 bits (must be preshifted by 28) (overlay: %s)", overlay_ucode->name);
    rspq_overlay_register_internal(overlay_ucode, overlay_id);
}

void rspq_overlay_share_state(rsp_ucode_t *overlay_dest, rsp_ucode_t *overlay_source)
{
    rspq_overlay_header_t *dsth = (rspq_overlay_header_t*)(overlay_dest->data + (rsp_queue_data_end - rsp_queue_data_start));
    rspq_overlay_header_t *srch = (rspq_overlay_header_t*)(overlay_source->data + (rsp_queue_data_end - rsp_queue_data_start));
    
    assertf((dsth->text_rdram & (1<<31)) == 0, 
        "Overlay %s is already using another shared state", overlay_dest->name);
    assertf(dsth->state_size <= srch->state_size, "Overlays %s (dest) and %s (src) have non-compatible state size (%d vs %d)",
        overlay_dest->name, overlay_source->name, dsth->state_size, srch->state_size);

    // Mark as using shared state. We need to mark it in the text segment because
    // it's what the rsp_queue ucode checks for.
    dsth->text_rdram |= (1<<31);

    // At overlay load time, copy state from source overlay into DMEM
    dsth->extraseg_rdram = srch->state_rdram;
    dsth->extraseg_size = dsth->state_size;
    dsth->extraseg_start = dsth->state_start;

    // At overlay save time, copy state from DMEM into source overlay
    dsth->state_rdram = srch->state_rdram;

    data_cache_hit_writeback_invalidate(dsth, sizeof(rspq_overlay_header_t));
}

void rspq_overlay_unregister(uint32_t overlay_id)
{
    assertf(overlay_id != 0, "Overlay 0 cannot be unregistered!");

    // Un-shift ID to convert to actual index again
    uint8_t ovl_id = overlay_id >> 28;
    rsp_ucode_t *ucode = rspq_overlay_ucodes[ovl_id];

    rspq_overlay_header_t *overlay_header = (rspq_overlay_header_t*)(ucode->data + (rsp_queue_data_end - rsp_queue_data_start));
    int slot_count = DIVIDE_CEIL(rspq_overlay_get_command_count(overlay_header), 16);

    // Reset the command base in the overlay header
    overlay_header->command_base = 0;
    data_cache_hit_writeback_invalidate(overlay_header, sizeof(rspq_overlay_header_t));

    // Remove all registered ids
    rspq_wait();
    vrsp_save_overlay_state();
    for (int i = 0; i < slot_count; i++) {
        rspq_data.rspq_ovl_table.data_rdram[ovl_id + i] = 0;
        rspq_data.rspq_ovl_table.idmap[ovl_id + i] = 0;
        vrsp_handlers[ovl_id + i] = NULL;
    }

    rspq_update_tables(false);
}

/**
 * @brief Switch to the next write buffer for the current RSP queue.
 * 
 * This function is invoked by #rspq_write when the current buffer is
 * full, that is, when the write pointer (#rspq_cur_pointer) reaches
 * the sentinel (#rspq_cur_sentinel). This means that we cannot safely
 * write any more new command in the buffer (the remaining bytes are less
 * than the maximum command size), and thus a new buffer must be configured.
 * 
 * If we're creating a block, we need to allocate a new buffer from the heap.
 * Otherwise, if we're writing into either the lowpri or the highpri queue,
 * we need to switch buffer (double buffering strategy), making sure the
 * other buffer has been already fully executed by the RSP.
 */
__attribute__((noinline))
void rspq_next_buffer(void) {
    // If we're creating a block
    if (rspq_block) {
        rspq_chain_next(&rspq_block_chain);
        return;
    }

    // If we're recording a queue
    if (rspq_queue_recording) {
        rspq_chain_next(&rspq_queue_recording->chain);
        return;
    }

    // We are about to switch buffer. If the debugging engine is activate,
    // it is a good time to run it, so that it does not accumulate too many
    // commands.
    if (rdpq_trace) rdpq_trace();

    // Poll the deferred list at least once per buffer switch. We will poll
    // more if we need to wait
    __rspq_deferred_poll();

    // Wait until the previous buffer is executed by the RSP.
    // We cannot write to it if it's still being executed.
    // FIXME: this should probably transition to a sync-point,
    // so that the kernel can switch away while waiting. Even
    // if the overhead of an interrupt is obviously higher.
    if (!(vsp_status & rspq_ctx->sp_status_bufdone)) {
        rspq_flush_internal();
        ACCT_SCOPE(ACCT_CAT_RSPQ) RSP_WAIT_LOOP(200) {
            __rspq_deferred_poll();
            if (vsp_status & rspq_ctx->sp_status_bufdone)
                break;
        }
    }
    vsp_write(rspq_ctx->sp_wstatus_clear_bufdone);

    // Switch current buffer
    rspq_ctx->buf_idx = 1-rspq_ctx->buf_idx;
    uint32_t *new = rspq_ctx->buffers[rspq_ctx->buf_idx];
    volatile uint32_t *prev = rspq_switch_buffer(new, rspq_ctx->buf_size, true);

    if (rspq_ctx == &lowpri) {
        // Stage the target in call slot 0 (RSPQ_LOWPRI_HANDOFF_SLOT).
        // All previously scheduled block calls have returned before this
        // top-level handoff can execute, so the slot is inactive. A highpri
        // request may safely preempt after this setup command because SIG_BUFDONE
        // has not been set yet.
        const uint32_t handoff_slot_offset = RSPQ_LOWPRI_HANDOFF_SLOT << 2;
        rspq_append2(prev, RSPQ_CMD_WRITE_WORD,
            offsetof(rsp_queue_t, rspq_pointer_stack) + handoff_slot_offset,
            PhysicalAddr(new));

        // SWAP_BUFFERS sets SIG_BUFDONE, loads the staged target, and jumps to
        // it without returning to RSPQ_Loop. Reusing the same slot for its
        // discarded return address is safe because the target is loaded first.
        rspq_append3(prev, RSPQ_CMD_SWAP_BUFFERS,
            handoff_slot_offset, handoff_slot_offset,
            rspq_ctx->sp_wstatus_set_bufdone);
    } else {
        // Highpri execution cannot itself be preempted, so we can use a smaller
        // sequence instead, which would be vulnerable to be preempted in the middle.
        rspq_append1(prev, RSPQ_CMD_WRITE_STATUS,
            rspq_ctx->sp_wstatus_set_bufdone);
        rspq_append1(prev, RSPQ_CMD_JUMP, PhysicalAddr(new));
    }
    assert(prev <= (uint32_t*)(rspq_ctx->buffers[1-rspq_ctx->buf_idx]) + rspq_ctx->buf_size);
    rspq_flush_internal();
}

__attribute__((noinline))
static void rspq_flush_internal(void)
{
    // Wake up the virtual RSP: it runs synchronously until the queue is drained.
    vsp_write(SP_WSTATUS_SET_SIG_MORE | SP_WSTATUS_CLEAR_HALT | SP_WSTATUS_CLEAR_BROKE);
    vrsp_run();
}

void rspq_flush(void)
{
    // If we are recording a block, flushes can be ignored.
    if (rspq_block || rspq_queue_recording) return;

    rspq_flush_internal();
    if (rdpq_trace) rdpq_trace();
}

void rspq_highpri_begin(void)
{
    assertf(!rspq_block, "cannot switch to highpri mode while creating a block");
    assertf(!rspq_queue_recording, "cannot switch to highpri mode while recording a queue");

    if (rspq_ctx == &highpri) {
        highpri_nesting++;
        return;
    }

    rspq_switch_context(&highpri);

    // Check if we're not at the beginning of the buffer. This avoids doing
    // OOB reads in the next check.
    if (rspq_cur_pointer != rspq_ctx->buffers[rspq_ctx->buf_idx]) {

        // If we're continuing on the same buffer another highpri sequence,
        // try to skip the highpri epilog and jump to the buffer continuation.
        // This is a small performance gain (the RSP doesn't need to exit and re-enter
        // the highpri mode) but it also allows to enqueue more than one highpri
        // sequence, since we only have a single SIG_HIGHPRI_REQUESTED and there
        // would be no way to tell the RSP "there are 3 sequences pending, so exit
        // and re-enter three times".
        // 
        // To skip the epilog we write single atomic words over  the epilog,
        // changing it with a JUMP to the buffer continuation. This operation
        // is completely safe because the RSP either see the memory before the
        // change (it sees the epilog) or after the change (it sees the new JUMP).
        // 
        // In the first case, it will run the epilog and then reenter the highpri
        // mode soon (as we're turning on SIG_HIGHPRI_REQUESTED anyway). In the 
        // second case, it's going to see the JUMP, skip the epilog and continue.
        // The SIG_HIGHPRI_REQUESTED bit will be set but this function, and reset
        // at the beginning of the new segment, but it doesn't matter at this point.
        if (rspq_cur_pointer[-3]>>24 == RSPQ_CMD_SWAP_BUFFERS) {
            volatile uint32_t *epilog = rspq_cur_pointer-4;
            rspq_append1(epilog, RSPQ_CMD_JUMP, PhysicalAddr(rspq_cur_pointer));
            rspq_append1(epilog, RSPQ_CMD_JUMP, PhysicalAddr(rspq_cur_pointer));
        }
    }

    // Clear SIG_HIGHPRI_REQUESTED and set SIG_HIGHPRI_RUNNING. This is normally done
    // automatically by RSP when entering highpri mode, but we want to still
    // add a command in case the previous epilog was skipped. Otherwise,
    // a dummy SIG_HIGHPRI_REQUESTED could stay on and eventually highpri
    // mode would enter once again.
    // Set SIG_HIGHPRI_REQUESTED *before* writing the WRITE_STATUS command that
    // clears it. If the RSP is already in highpri mode and caught up with the
    // write cursor, it can execute the appended WRITE_STATUS within a few
    // cycles of the append; with the old order (append first, then set), the
    // clear could be consumed before the set landed, leaving a dangling
    // REQUESTED that made the RSP re-enter highpri after the final epilog and
    // park forever on the empty queue with SIG_HIGHPRI_RUNNING set (deadlocking
    // rspq_highpri_sync). Setting the signal first closes the race: the RSP
    // cannot execute the clear before it is written, which is after the set.
    vsp_write(SP_WSTATUS_SET_SIG_HIGHPRI_REQUESTED);
    rspq_append1(rspq_cur_pointer, RSPQ_CMD_WRITE_STATUS,
        SP_WSTATUS_CLEAR_SIG_HIGHPRI_REQUESTED | SP_WSTATUS_SET_SIG_HIGHPRI_RUNNING);
    rspq_flush_internal();
}

void rspq_highpri_end(void)
{
    assertf(rspq_ctx == &highpri, "not in highpri mode");

    if (highpri_nesting) {
        highpri_nesting--;
        return;
    }

    // Write the highpri epilog. The epilog starts with a JUMP to the next
    // instruction because we want to force the RSP to reload the buffer
    // from RDRAM in case the epilog has been overwritten by a new highpri
    // queue (see rspq_highpri_begin).
    rspq_append1(rspq_cur_pointer, RSPQ_CMD_JUMP, PhysicalAddr(rspq_cur_pointer+1));
    rspq_append3(rspq_cur_pointer, RSPQ_CMD_SWAP_BUFFERS,
        RSPQ_LOWPRI_CALL_SLOT<<2, RSPQ_HIGHPRI_CALL_SLOT<<2,
        SP_WSTATUS_CLEAR_SIG_HIGHPRI_RUNNING);
    rspq_flush_internal();
    rspq_switch_context(&lowpri);
}

void rspq_highpri_sync(void)
{
    // Make sure the RSP is running, otherwise we might be blocking forever.
    // This also clears HALT, so that the check below cannot be fooled by a
    // halted state that predates the commands we are waiting for.
    rspq_flush_internal();

    if (rspq_ctx == &highpri) {
        // We are in the middle of building a highpri sequence, so its epilog
        // (the only thing that clears SIG_HIGHPRI_RUNNING) has not been written
        // yet and the wait below would never be satisfied. Wait on a different
        // condition: an unterminated highpri queue simply ends at our write
        // pointer, where the RSP finds the queue terminator and halts itself
        // (see RSPQCmd_WaitNewInput in rsp_queue.inc). So the halt means that
        // everything written so far has been executed. DMA status must be
        // checked too: "break" also runs while an asynchronous transfer
        // started by the last command is still in flight.
        // The virtual RSP already ran everything written so far.
        return;
    }

    ACCT_SCOPE(ACCT_CAT_RSPQ) RSP_WAIT_LOOP(200) {
        __rspq_deferred_poll();
        if (!(vsp_status & (SP_STATUS_SIG_HIGHPRI_REQUESTED | SP_STATUS_SIG_HIGHPRI_RUNNING)))
            break;
        vrsp_run();
    }
}

bool rspq_in_highpri(void)
{
    return (rspq_ctx == &highpri);
}

/** Invoke all #rspq_block_atexit callbacks and free the list nodes. */
static void rspq_block_free_atexit_chain(rspq_block_t *block)
{
    rspq_block_cb_t *cb = block->atexit;
    while (cb) {
        cb->cb(cb->ctx);
        rspq_block_cb_t *next = cb->next;
        free(cb);
        cb = next;
    }
    block->atexit = NULL;
}

void rspq_block_begin_reuse(rspq_block_t *reuse_block)
{
    assertf(!rspq_block, "a block was already being created");
    assertf(rspq_ctx != &highpri, "cannot create a block in highpri mode");
    assertf(!rspq_queue_recording, "cannot create a block while recording a queue");

    rspq_block_t *block;
    int block_size = RSPQ_BLOCK_MIN_SIZE;

    if (!reuse_block) {
        block = malloc_uncached(sizeof(rspq_block_t) + block_size*sizeof(uint32_t));
        assertf(block, "Out of memory");
        block->nesting_level = 0;
        block->min_ph_level = RSPQ_MAX_BLOCK_NESTING_LEVEL;
        block->rdp_block = NULL;
        block->atexit = NULL;
        rspq_chain_init(&rspq_block_chain, block->cmds, block_size);
    } else {
        block = reuse_block;
        hle_replay_block_gone(block->cmds);
        rspq_block_free_atexit_chain(block);
        block->nesting_level = 0;
        block->min_ph_level = RSPQ_MAX_BLOCK_NESTING_LEVEL;
        rspq_chain_reset(&rspq_block_chain, block->cmds, block_size);
    }

    // Switch to the block buffer. From now on, all rspq_writes will
    // go into the block.
    rspq_switch_context(NULL);
    rspq_switch_buffer(block->cmds, block_size, false);
    rspq_block_chain.cur = rspq_cur_pointer;
    rspq_block_chain.sentinel = rspq_cur_sentinel;

    rspq_block = block;

    if (block->rdp_block)
        __rdpq_block_recycle(block->rdp_block);
    else
        __rdpq_block_begin();
}

rspq_block_t* rspq_block_end(void)
{
    assertf(rspq_block, "a block was not being created");

    // Terminate the block with a RET command, encoding
    // the nesting level which is used as stack slot by RSP.
    rspq_append1(rspq_cur_pointer, RSPQ_CMD_RET, rspq_block->nesting_level<<2);

    // Switch back to the normal display list
    rspq_switch_context(&lowpri);

    // Save pointer to rdpq block (if any)
    rspq_block->rdp_block = __rdpq_block_end();

#ifdef HLE_TEST_PERF
    { extern uint32_t hle_blk_addr[512]; extern int hle_blk_n; if (hle_blk_n < 512) hle_blk_addr[hle_blk_n++] = PhysicalAddr(rspq_block->cmds); }
#endif
    hle_replay_block_new(rspq_block->cmds);
    // Return the created block
    rspq_block_t *b = rspq_block;
    rspq_block = NULL;
    return b;
}

void rspq_block_free(rspq_block_t *block)
{
    hle_replay_block_gone(block->cmds);
    // Free RDP blocks first
    __rdpq_block_free(block->rdp_block);

    rspq_chain_free(block->cmds, RSPQ_BLOCK_MIN_SIZE);

    // Lastly, invoke callbacks (in reverse order of registration)
    rspq_block_free_atexit_chain(block);

    free_uncached(block);
}

void rspq_block_set_placeholder(
  rspq_block_t *ph,
  rspq_block_t *ph_target
) {
  uint32_t slot = (uint32_t)ph;
  assertf(slot < RSPQ_BLOCK_PLACEHOLDER_COUNT, "Invalid placeholder: %08lX", slot);
  slot = (RSPQ_MAX_BLOCK_NESTING_LEVEL-1) - slot;

  assertf(ph_target->nesting_level == 0, "Nested blocks cannot be used as placeholders");

  uint32_t ptr_stack = offsetof(rsp_queue_t, rspq_pointer_stack);
  rspq_int_write(RSPQ_CMD_WRITE_WORD, 
    ptr_stack + (slot << 2), 
    PhysicalAddr(ph_target->cmds)
  );
}

void rspq_block_run(rspq_block_t *block)
{
    // TODO: add support for block execution in highpri mode. This would be
    // possible by allocating another range of nesting levels (eg: 8-16) to use
    // in highpri mode (to avoid stepping on the call stack of lowpri). This
    // would basically mean that a block can either work in highpri or in lowpri
    // mode, but it might be an acceptable limitation.
    // NOTE: during highpri mode, the slot 0 (RSPQ_LOWPRI_HANDOFF_SLOT) might be
    // in-use if highpri preempted lowpri exactly during a buffer swap, so make
    // sure to avoid using it.
    assertf(rspq_ctx != &highpri, "block run is not supported in highpri mode");

    if((uint32_t)block < RSPQ_BLOCK_PLACEHOLDER_COUNT)
    {
      assertf(rspq_block, "Calling a placeholder is only supported inside a block");
      uint32_t slot = (RSPQ_MAX_BLOCK_NESTING_LEVEL-1) - (uint32_t)block;
      
      if(slot < rspq_block->min_ph_level) {
        rspq_block->min_ph_level = slot;
      }

      uint32_t dmem_ph_addr = offsetof(rsp_queue_t, rspq_pointer_stack);
      dmem_ph_addr += slot << 2;

      // always assume the called block has no further nesting, this is asserted in 'rspq_block_set_placeholder'.
      const uint32_t block_nesting = 0;
    
      rspq_int_write(RSPQ_CMD_CALL, dmem_ph_addr, (block_nesting << 2) | (1<<31));

      // bump up the current blocks level, it only has to make room for one level once
      if (rspq_block->nesting_level == 0) {
        rspq_block->nesting_level = 1;
      }

      // set RDP to unknown state, since we don't know yet what it may contain
      __rdpq_block_run_maybe_rdp();
      return;
    }

    assertf(block->nesting_level < block->min_ph_level, "Block nesting level overlaps with used placeholders");

    // Write the CALL op. The second argument is the nesting level
    // which is used as stack slot in the RSP to save the current
    // pointer position.
    rspq_int_write(RSPQ_CMD_CALL, PhysicalAddr(block->cmds), block->nesting_level << 2);

    // If this is CALL within the creation of a block, update
    // the nesting level. A block's nesting level must be bigger
    // than the nesting level of all blocks called from it.
    if (rspq_block && rspq_block->nesting_level <= block->nesting_level) {
        rspq_block->nesting_level = block->nesting_level + 1;
        assertf(rspq_block->nesting_level < RSPQ_MAX_BLOCK_NESTING_LEVEL,
            "reached maximum number of nested block runs");
    }
    if (rspq_queue_recording && rspq_queue_recording->nesting_level <= block->nesting_level) {
        rspq_queue_recording->nesting_level = block->nesting_level + 1;
        assertf(rspq_queue_recording->nesting_level < RSPQ_MAX_BLOCK_NESTING_LEVEL,
            "reached maximum number of nested queue runs");
    }

    // Notify rdpq engine we have run a block
    if(block->rdp_block) {
      __rdpq_block_run_with_rdp(block->rdp_block);
    } else {
      __rdpq_block_run_no_rdp();
    }
}

void rspq_block_run_rsp(int nesting_level)
{
    __rdpq_block_run_no_rdp();
    if (rspq_block && rspq_block->nesting_level <= nesting_level) {
        rspq_block->nesting_level = nesting_level + 1;
        assertf(rspq_block->nesting_level < RSPQ_MAX_BLOCK_NESTING_LEVEL,
            "reached maximum number of nested block runs");
    }    
}

void rspq_block_atexit(void (*cb)(void*), void* ctx)
{
    assertf(rspq_block, "no block is being created");
    rspq_block_cb_t *new_cb = malloc(sizeof(rspq_block_cb_t));
    assertf(new_cb, "Out of memory");
    new_cb->cb = cb;
    new_cb->ctx = ctx;
    // Insert at the front of the list.
    // This means that iteration will happen in reverse order of insertion.
    new_cb->next = rspq_block->atexit;
    rspq_block->atexit = new_cb;
}

rspq_queue_t* rspq_queue_create(void)
{
    rspq_queue_t *q = malloc_uncached(sizeof(rspq_queue_t) + RSPQ_BLOCK_MIN_SIZE * sizeof(uint32_t));
    assertf(q, "Out of memory");

    rspq_chain_init(&q->chain, q->cmds, RSPQ_BLOCK_MIN_SIZE);
    q->run_start = q->chain.first_chunk;
    q->nesting_level = 0;

    __rdpq_tracking_state_reset(&q->rdpq_tracking);
    return q;
}

void rspq_queue_switch(rspq_queue_t* q)
{
    assertf(rspq_ctx != &highpri, "cannot switch queue in highpri mode");
    assertf(!rspq_block, "cannot switch queue while creating a block");

    if (q == rspq_queue_recording)
        return;

    if (rspq_queue_recording) {
        rspq_queue_recording->chain.cur = rspq_cur_pointer;
        rspq_queue_recording->chain.sentinel = rspq_cur_sentinel;
        // there is already a queue active we want to move away from now,
        // save its current tracking back to the queue,
        // and restore what the queue backed up
        SWAP(rdpq_tracking, rspq_queue_recording->rdpq_tracking);
    }

    if (!q) {
        rspq_queue_recording = NULL;
        rspq_switch_context(&lowpri);
        return;
    }
    
    // we want to move into a queue, take the tracking state from the queue
    // and backup the current main one
    SWAP(rdpq_tracking, q->rdpq_tracking);
    rspq_queue_recording = q;

    if (rspq_ctx != NULL)
        rspq_switch_context(NULL);
    rspq_cur_pointer = q->chain.cur;
    rspq_cur_sentinel = q->chain.sentinel;
}

void rspq_queue_run(rspq_queue_t* q)
{
    assertf(q, "queue is NULL");
    assertf(rspq_ctx != &highpri, "queue run is not supported in highpri mode");
    assertf(!rspq_block, "queue run is not supported while creating a block");

    // Switch to the lowpri context.
    rspq_queue_t *prev_recording = rspq_queue_recording;
    if (prev_recording)
        rspq_queue_switch(NULL);

    // Terminate the current chunk with a RET command (do not advance cur).
    rspq_append1(q->chain.cur, RSPQ_CMD_RET, q->nesting_level << 2);

    // Enqueue a CALL in the lowpri context to run the queue.
    rspq_int_write(RSPQ_CMD_CALL, PhysicalAddr(q->run_start), q->nesting_level << 2);

    // Update run_start so that the next call will start from the current position.
    q->run_start = q->chain.cur;

    // Switch back to the recording context.
    if (prev_recording)
        rspq_queue_switch(prev_recording);

    // after the queue was executed, we can take its tracking state going forward
    rdpq_tracking = q->rdpq_tracking;
    __rdpq_tracking_state_reset(&q->rdpq_tracking);
}

void rspq_queue_clear(rspq_queue_t* q)
{
    assertf(q, "queue is NULL");

    rspq_chain_reset(&q->chain, q->chain.first_chunk, RSPQ_BLOCK_MIN_SIZE);
    q->nesting_level = 0;
    q->run_start = q->chain.first_chunk;
    __rdpq_tracking_state_reset(&q->rdpq_tracking);

    if (q == rspq_queue_recording) {
        rspq_cur_pointer = q->chain.cur;
        rspq_cur_sentinel = q->chain.sentinel;
    }
}

void rspq_queue_destroy(rspq_queue_t* q)
{
    assertf(q, "queue is NULL");
    assertf(q != rspq_queue_recording, "cannot destroy queue while recording it");

    rspq_chain_free(q->chain.first_chunk, RSPQ_BLOCK_MIN_SIZE);
    free_uncached(q);
}

void rspq_noop()
{
    // WRITE_STATUS performs a write to COP0_SP_STATUS, which does nothing if the argument is zero
    rspq_int_write(RSPQ_CMD_WRITE_STATUS, 0);
}

rspq_syncpoint_t rspq_syncpoint_new(void)
{   
    assertf(rspq_ctx != &highpri, "cannot create syncpoint in highpri mode");
    assertf(!rspq_block, "cannot create syncpoint in a block");
    assertf(!rspq_queue_recording, "cannot create syncpoint in a queue");
    assertf(rspq_ctx != &highpri, "cannot create syncpoint in highpri mode");

    // To create a syncpoint, schedule a CMD_TEST_WRITE_STATUS command that:
    //   1. Wait for SP_STATUS_SIG_SYNCPOINT to go zero. This is cleared in
    //      the RSP interrupt routine and basically make sure that any other
    //      pending interrupt had been acknowledged. Otherwise, we might
    //      end up coalescing multiple RSP interrupts, and thus missing
    //      syncpoints (as we need exactly one handled interrupt per syncpoint).
    //   2. Write SP_STATUS with SP_WSTATUS_SET_SIG_SYNCPOINT and SP_WSTATUS_SET_INTR,
    //      forcing a new RSP interrupt to be generated. The interrupt routine
    //      (#rspq_sp_interrupt) will notice the SP_STATUS_SIG_SYNCPOINT and know
    //      that the interrupt has been generated for a syncpoint.
    rspq_int_write(RSPQ_CMD_TEST_WRITE_STATUS, 
        SP_WSTATUS_SET_INTR | SP_WSTATUS_SET_SIG_SYNCPOINT,
        SP_STATUS_SIG_SYNCPOINT);
    return ++rspq_syncpoints_genid;
}

bool rspq_syncpoint_check(rspq_syncpoint_t sync_id) 
{
    int difference = (int)((uint32_t)(sync_id) - (uint32_t)(__rspq_syncpoints_done[0]));
    return difference <= 0;
}

void rspq_syncpoint_wait(rspq_syncpoint_t sync_id)
{
    if (rspq_syncpoint_check(sync_id))
        return;

    assertf(get_interrupts_state() == INTERRUPTS_ENABLED,
        "deadlock: interrupts are disabled");

    // Make sure the RSP is running, otherwise we might be blocking forever.
    rspq_flush_internal();

    // Spinwait until the the syncpoint is reached.
    // TODO: with the kernel, it will be possible to wait for the RSP interrupt
    // to happen, without spinwaiting.
    ACCT_SCOPE(ACCT_CAT_RSPQ) RSP_WAIT_LOOP(200) {
        __rspq_deferred_poll();
        if (rspq_syncpoint_check(sync_id))
            break;
    }
}

/**
 * @brief Polls the deferred calls list, calling callbacks ready to be called.
 * 
 * This function will check the deferred call list and if there is one callback
 * ready to be called, it will call it and remove it from the list.
 * 
 * The function will process maximum one callback per call, so that it does
 * not steal too much CPU time.
 * 
 * @return true   if there are still callbacks to be processed
 * @return false  if there are no more callbacks to be processed
 */ 
bool __rspq_deferred_poll(void)
{
    rspq_deferred_call_t *prev = NULL, *cur =  defcalls_head;
    while (cur != NULL) {
        rspq_deferred_call_t *next = cur->next;

        // Since the list is chronologically sorted, once we reach the first
        // call that is still waiting for its RSP checkpoint, we can stop.
        if (!rspq_syncpoint_check(cur->sync))
            break;

        // If this call requires waiting for SYNC_FULL, check if we reached it.
        // Otherwise, jsut skio it and go through the list: maybe a later callback
        // does not require RDP and can be called.
        if (cur->flags & RSPQ_DCF_WAITRDP) {
            int difference = (int)((uint32_t)(cur->sync) - (uint32_t)(__rdpq_syncpoint_at_syncfull));
            if (difference <= 0)
                cur->flags &= ~RSPQ_DCF_WAITRDP;
        }

        // If this call does not require waiting for next SYNC_FULL, call it.
        if (!(cur->flags & RSPQ_DCF_WAITRDP)) {
            // Call the deferred calllback. Account the time to user-time as
            // this is actually a non-kernel activity that's just deferred.
            ACCT_SCOPE(ACCT_CAT_USER) cur->func(cur->arg);

            // Remove it from the list (possibly updating the head/tail pointer)
            if (prev)
                prev->next = next;
            else
                defcalls_head = next;
            if (!next)
                defcalls_tail = prev;
            defcalls_count--;
            free(cur);

            if (defcalls_count < defcalls_run_threshold)
                break;
        } else {
            prev = cur;
        }

        cur = next;
    }

    defcalls_run_threshold = (defcalls_run_threshold * 4 / 8) + (defcalls_count * 4 / 8);

    return defcalls_head != NULL;
}

rspq_syncpoint_t __rspq_call_deferred(void (*func)(void *), void *arg, bool waitrdp)
{
    assertf(rspq_ctx != &highpri, "cannot defer in highpri mode");
    assertf(!rspq_block, "cannot defer in a block");
    assertf(!rspq_queue_recording, "cannot defer in a queue");

    // Allocate a new deferred call
    rspq_deferred_call_t *call = malloc(sizeof(rspq_deferred_call_t));
    assertf(call, "Out of memory");
    call->func = func;
    call->arg = arg;
    call->next = NULL;
    call->sync = rspq_syncpoint_new();
    if (waitrdp)
        call->flags |= RSPQ_DCF_WAITRDP;

    // Add it to the list of deferred calls
    if (defcalls_tail) {
        defcalls_tail->next = call;
    } else {
        defcalls_head = call;
    }
    defcalls_tail = call;
    defcalls_count++;

    return call->sync;
}

rspq_syncpoint_t rspq_syncpoint_new_cb(void (*func)(void *), void *arg)
{
    return __rspq_call_deferred(func, arg, false);
}

void rspq_wait(void)
{
    assertf(!rspq_queue_recording, "cannot wait in a queue");
    assertf(!rspq_block, "cannot wait in a block");
    assertf(rspq_ctx != &highpri, "cannot wait in highpri mode");
    
    // Check if the RDPQ module was initialized.
    if (__rdpq_inited) {
        // If so, a full sync requires also waiting for RDP to finish.
        rdpq_fence();

        // Also force a buffer switch to go back to dynamic buffer. This is useful
        // in the case the RDP is still pointing to a static buffer (after a block
        // is just finished). This allows the user to safely free the static buffer
        // after rspq_wait(), as intuition would suggest.
        rspq_int_write(RSPQ_CMD_RDP_SET_BUFFER, 0, 0, 0);
    }
    
    // Wait until RSP has finished processing the queue
    rspq_syncpoint_wait(rspq_syncpoint_new());

    // Update the tracing engine (if enabled)
    if (rdpq_trace) rdpq_trace();

    // Make sure to process all deferred calls. Since this is a full sync point,
    // it makes sense to give this guarantee to the user.
    ACCT_SCOPE(ACCT_CAT_RSPQ) RSP_WAIT_LOOP(500) {
        if (!__rspq_deferred_poll())
            break;
    }

}

/// @cond
void rspq_signal(uint32_t signal)
{
    const uint32_t allowed_mask = SP_WSTATUS_CLEAR_SIG0|SP_WSTATUS_SET_SIG0;
    assertf((signal & allowed_mask) == signal, "rspq_signal called with a mask that contains bits outside SIG0: %lx", signal);

    rspq_int_write(RSPQ_CMD_WRITE_STATUS, signal);
}
/// @endcond

static void rspq_dma(void *rdram_addr, uint32_t dmem_addr, uint32_t len, uint32_t flags)
{
    rspq_int_write(RSPQ_CMD_DMA, PhysicalAddr(rdram_addr), dmem_addr, len, flags);
}

void rspq_dma_to_rdram(void *rdram_addr, uint32_t dmem_addr, uint32_t len, bool is_async)
{
    rspq_dma(rdram_addr, dmem_addr, len - 1, 0xFFFF8000 | (is_async ? 0 : SP_STATUS_DMA_BUSY | SP_STATUS_DMA_FULL));
}

void rspq_dma_to_dmem(uint32_t dmem_addr, void *rdram_addr, uint32_t len, bool is_async)
{
    rspq_dma(rdram_addr, dmem_addr, len - 1, is_async ? 0 : SP_STATUS_DMA_BUSY | SP_STATUS_DMA_FULL);
}

/* Extern inline instantiations. */
extern inline void rspq_block_begin(void);
extern inline rspq_write_t rspq_write_begin(uint32_t ovl_id, uint32_t cmd_id, int size);
extern inline void rspq_write_arg(rspq_write_t *w, uint32_t value);
extern inline void rspq_write_end(rspq_write_t *w);
extern inline void rspq_call_deferred(void (*func)(void *), void *arg);

/* ===========================================================================
 * Virtual RSP: a C port of the rsp_queue.inc command loop. Commands are
 * consumed straight from RDRAM; overlay commands are dispatched to the C
 * reimplementations of each microcode (hle/ovl_*.c).
 * ======================================================================== */

uint8_t *__vrsp_img;
uint32_t __vrsp_state_start, __vrsp_state_end;
static uint8_t *vrsp_imgs[RSPQ_MAX_OVERLAYS];      // per-overlay DMEM image
static uint8_t vrsp_cmdsize[RSPQ_MAX_OVERLAYS][64]; // command sizes (bytes) per overlay
static bool vrsp_img_loaded[RSPQ_MAX_OVERLAYS];    // image holds the live state
static bool vrsp_running;

static vrsp_handler_t vrsp_find_handler(rsp_ucode_t *ucode)
{
    const char *n = ucode->name;
    if (!strcmp(n, "rsp_rdpq"))   return ovl_rdpq;
    if (!strcmp(n, "rsp_tiny3d")) return ovl_t3d;
    if (!strcmp(n, "rsp_tinypx")) return ovl_tpx;
    if (!strcmp(n, "rsp_mixer"))  return ovl_mixer;
    hle_warn_once(n, "no HLE implementation of RSP overlay %s; its commands are ignored", n);
    return NULL;
}

static uint32_t vrsp_common_size(void)
{
    return rsp_queue_data_end - rsp_queue_data_start;
}

static rspq_overlay_header_t* vrsp_header(int ovl)
{
    return (rspq_overlay_header_t*)(vrsp_imgs[ovl] + vrsp_common_size());
}

static void vsp_write(uint32_t w)
{
    uint32_t s = vsp_status;
    if (w & SP_WSTATUS_CLEAR_HALT)  s &= ~SP_STATUS_HALTED;
    if (w & SP_WSTATUS_SET_HALT)    s |= SP_STATUS_HALTED;
    if (w & SP_WSTATUS_CLEAR_BROKE) s &= ~SP_STATUS_BROKE;
    for (int k = 0; k < 8; k++) {
        if (w & (1u << (9 + 2*k)))  s &= ~(1u << (7 + k));
        if (w & (1u << (10 + 2*k))) s |=  (1u << (7 + k));
    }
    vsp_status = s;
    // The real RSP raises an interrupt here; we deliver it synchronously.
    if (w & SP_WSTATUS_SET_INTR)
        rspq_sp_interrupt();
}

/** Makes `ovl` the current overlay, loading its state on first use. */
static void vrsp_load_overlay(int ovl)
{
    vrsp_cur_ovl = ovl;
    if (!vrsp_imgs[ovl]) {
        vrsp_imgs[ovl] = memalign(16, 4096);
        assertf(vrsp_imgs[ovl], "HLE: out of memory");
    }
    __vrsp_img = vrsp_imgs[ovl];
    rspq_overlay_header_t *h = vrsp_header(ovl);
    if (!vrsp_img_loaded[ovl]) {
        rsp_ucode_t *uc = rspq_overlay_ucodes[ovl];
        rspq_overlay_header_t *rh = (rspq_overlay_header_t*)(uc->data + vrsp_common_size());
        for (int i = 0; i < 64 && rh->commands[i]; i++)
            vrsp_cmdsize[ovl][i] = (rh->commands[i] >> 8) & RSPQ_DESCRIPTOR_SIZE_MASK;
        uint32_t common = vrsp_common_size();
        memcpy(__vrsp_img + common, uc->data + common,
               (uint8_t*)uc->data_end - (uint8_t*)uc->data - common);
        memcpy(__vrsp_img + (h->state_start & 0xFFF), hle_rdram(h->state_rdram),
               h->state_size + 1);
        if (h->text_rdram & (1u << 31))
            memcpy(__vrsp_img + (h->extraseg_start & 0xFFF), hle_rdram(h->extraseg_rdram),
                   h->extraseg_size + 1);
        vrsp_img_loaded[ovl] = true;
    }
    __vrsp_state_start = h->state_start & 0xFFF;
    __vrsp_state_end = __vrsp_state_start + h->state_size + 1;
}

/** Writes back the state of every overlay to RDRAM (like an overlay switch would). */
static void vrsp_save_overlay_state(void)
{
    for (int i = 1; i < RSPQ_MAX_OVERLAYS; i++) {
        if (!vrsp_img_loaded[i]) continue;
        rspq_overlay_header_t *h = vrsp_header(i);
        memcpy(hle_rdram(h->state_rdram), vrsp_imgs[i] + (h->state_start & 0xFFF),
               h->state_size + 1);
        vrsp_img_loaded[i] = false;
    }
}

/** DMEM address seen by the builtin commands (DMA/WRITE_WORD). */
static uint8_t* vrsp_dmem_addr(uint32_t addr)
{
    addr &= 0xFFF;
    if (addr < vrsp_common_size() || !vrsp_cur_ovl)
        return __vrsp_dmem + addr;
    return vrsp_imgs[vrsp_cur_ovl] + addr;
}

// ---- RDP output buffers (RSPQCmd_RdpSetBuffer / RSPQCmd_RdpAppendBuffer) ----

void vrsp_rdp_append_buffer(uint32_t a0)
{
    rspq_data.rspq_rdp_current = a0;
    vrdp_set_end(a0);
}

void vrsp_rdp_set_buffer(uint32_t a0, uint32_t a1, uint32_t a2)
{
    rspq_data.rspq_rdp_sentinel = a2;
    if (!a2) return;
    rspq_data.rdp_syncfull_ongoing = 0;
    vrdp_set_end(rspq_data.rspq_rdp_current);
    vrdp_set_start(a1);
    vrsp_rdp_append_buffer(a0);
}

void vrsp_rdpq_send(const uint32_t *words, int nbytes)
{
    if (!nbytes) return;
    uint32_t cur = rspq_data.rspq_rdp_current & 0xFFFFFF;
    uint32_t sentinel = rspq_data.rspq_rdp_sentinel;
    hle_rp_rdp_send(nbytes);
    if ((int32_t)(sentinel - nbytes) < (int32_t)cur) {
        hle_rp_bad();
        // Switch to the other dynamic buffer
        cur = rspq_data.rspq_rdp_buffers[1];
        rspq_data.rspq_rdp_buffers[1] = rspq_data.rspq_rdp_buffers[0];
        rspq_data.rspq_rdp_buffers[0] = cur;
        rspq_data.rspq_rdp_sentinel = cur + RDPQ_DYNAMIC_BUFFER_SIZE;
        rspq_data.rdp_syncfull_ongoing = 0;
        vrdp_set_end(rspq_data.rspq_rdp_current);
        vrdp_set_start(cur);
    }
    // Commands are translated directly instead of being written to RDRAM,
    // as if they had been written at `cur`.
    vrdp_sync_cursor(cur);
    vrdp_direct((const uint64_t*)words, nbytes);
    rspq_data.rspq_rdp_current = cur + nbytes;
}

// ---- Command loop ----

#ifdef HLE_TEST_PERF
uint32_t hle_blk_addr[512], hle_blk_mask, hle_blk_t0, hle_blk_ticks, hle_blk_calls; int hle_blk_n;
uint32_t __hle_perf_ticks, __hle_perf_cmd[16][64], __hle_perf_cnt[16][64], hle_rm_calls[4], hle_fin_ticks;
void hle_perf_dump(uint32_t total)
{
    for (int o = 0; o < 16; o++) for (int c = 0; c < 64; c++) {
        if (__hle_perf_cmd[o][c] * 100ULL / total >= 1)
            debugf("[hle]   ovl %s cmd %02x: %lu%% (%lu calls)\n", o == 0 ? "rspq" : rspq_overlay_ucodes[o] ? rspq_overlay_ucodes[o]->name : "?", c,
                (unsigned long)(__hle_perf_cmd[o][c] * 100ULL / total), (unsigned long)__hle_perf_cnt[o][c]);
        __hle_perf_cmd[o][c] = __hle_perf_cnt[o][c] = 0;
    }
    extern uint32_t hle_vl_ticks[10], hle_vl_verts, hle_vl_amb, hle_vl_zrow, hle_vl_fog;
    debugf("[hle]   vert_load setup: before fog %lu%%, fog plane %lu%%\n", (unsigned long)(hle_vl_ticks[4] * 100ULL / total), (unsigned long)(hle_vl_ticks[5] * 100ULL / total)); hle_vl_ticks[4] = hle_vl_ticks[5] = 0;
    debugf("[hle]   vert_load: %lu ambient tables, %lu z rows, %lu fogged loads\n", (unsigned long)hle_vl_amb, (unsigned long)hle_vl_zrow, (unsigned long)hle_vl_fog);
    hle_vl_amb = hle_vl_zrow = hle_vl_fog = 0;
    debugf("[hle]   vert_load: setup %lu%% begin %lu%% loop %lu%% vtx %lu%%, %lu verts\n", (unsigned long)(hle_vl_ticks[0] * 100ULL / total), (unsigned long)(hle_vl_ticks[1] * 100ULL / total),
           (unsigned long)(hle_vl_ticks[2] * 100ULL / total), (unsigned long)(hle_vl_ticks[3] * 100ULL / total), (unsigned long)hle_vl_verts);
    hle_vl_ticks[0] = hle_vl_ticks[1] = hle_vl_ticks[2] = hle_vl_ticks[3] = hle_vl_verts = 0;
    debugf("[hle]   vert_load hits: find %lu%% begin %lu%% vtx %lu%%\n", (unsigned long)(hle_vl_ticks[6] * 100ULL / total), (unsigned long)(hle_vl_ticks[7] * 100ULL / total), (unsigned long)(hle_vl_ticks[8] * 100ULL / total));
    hle_vl_ticks[6] = hle_vl_ticks[7] = hle_vl_ticks[8] = 0;
    debugf("[hle]   inside recorded blocks: %lu%% (%lu block runs, %d blocks)\n", (unsigned long)(hle_blk_ticks * 100ULL / total), (unsigned long)hle_blk_calls, hle_blk_n);
    hle_blk_ticks = hle_blk_calls = 0;
    extern uint32_t hle_rp_stats[8];
    debugf("[hle]   replay: %lu replayed, %lu recorded, %lu unreplayable, %lu no match, check %lu ok %lu BAD\n", (unsigned long)hle_rp_stats[0],
           (unsigned long)hle_rp_stats[1], (unsigned long)hle_rp_stats[2], (unsigned long)hle_rp_stats[3], (unsigned long)hle_rp_stats[4], (unsigned long)hle_rp_stats[5]);
    memset(hle_rp_stats, 0, sizeof(hle_rp_stats));
    extern uint32_t hle_rm_calls[4], hle_fin_ticks;
    debugf("[hle]   rdpq finalize (all commands): %lu%%\n", (unsigned long)(hle_fin_ticks * 100ULL / total)); hle_fin_ticks = 0;
    debugf("[hle]   modify other modes: %lu plain, %lu render mode (%lu memo hits), %lu mode/combine commands dropped\n",
           (unsigned long)hle_rm_calls[0], (unsigned long)hle_rm_calls[1], (unsigned long)hle_rm_calls[2], (unsigned long)hle_rm_calls[3]);
    memset(hle_rm_calls, 0, sizeof(hle_rm_calls));
    extern uint32_t hle_vc_hits, hle_vc_misses, hle_vc_resets, hle_vc_peak, hle_vc_strip_hits;
    debugf("[hle]   vertex cache: %lu hits, %lu strip hits, %lu misses, %lu resets, peak %lu KB\n", (unsigned long)hle_vc_hits, (unsigned long)hle_vc_strip_hits,
           (unsigned long)hle_vc_misses, (unsigned long)hle_vc_resets, (unsigned long)hle_vc_peak / 1024);
    hle_vc_hits = hle_vc_strip_hits = hle_vc_misses = hle_vc_resets = 0;
}
#endif

static void vrsp_run(void)
{
    if (vrsp_running) return;   // re-entered from a handler (e.g. via rspq_wait)
    vrsp_running = true;
#ifdef HLE_TEST_PERF
    uint32_t t0 = TICKS_READ();
#endif
    vsp_status &= ~SP_STATUS_HALTED;

    while (1) {
        // Highpri requested and not yet running: switch queues (rspq_switch_highpri)
        if ((vsp_status & (SP_STATUS_SIG_HIGHPRI_REQUESTED | SP_STATUS_SIG_HIGHPRI_RUNNING))
                == SP_STATUS_SIG_HIGHPRI_REQUESTED) {
            vsp_write(SP_WSTATUS_CLEAR_SIG_HIGHPRI_REQUESTED | SP_WSTATUS_SET_SIG_HIGHPRI_RUNNING);
            uint32_t target = PSTACK(RSPQ_HIGHPRI_CALL_SLOT);
            hle_replay_abort();
            PSTACK(RSPQ_LOWPRI_CALL_SLOT) = vrsp_ptr;
            vrsp_ptr = target & 0xFFFFFF;
            continue;
        }

        const uint32_t *a = hle_rdram(vrsp_ptr);
        uint32_t a0 = a[0];
        if (a0 == 0) {
            // RSPQCmd_WaitNewInput: caught up with the CPU
            vsp_status &= ~SP_STATUS_SIG_MORE;
            break;
        }

        uint32_t ovl_id = a0 >> 28;
        uint32_t cmd_byte = a0 >> 24;
        if (ovl_id == 0) {
#ifdef HLE_TEST_PERF
            uint32_t c0 = TICKS_READ();
#endif
            static const uint8_t sizes[] = { 0, 8, 4, 8, 4, 16, 4, 12, 8, 12, 4 };
            assertf(cmd_byte < sizeof(sizes), "HLE: invalid rspq command %08lx at %08lx", a0, vrsp_ptr);
            uint32_t next = vrsp_ptr + sizes[cmd_byte];
            switch (cmd_byte) {
            case RSPQ_CMD_WRITE_WORD:
                hle_rp_bad();
                *(uint32_t*)vrsp_dmem_addr(a0) = a[1];
                break;
            case RSPQ_CMD_JUMP:
                next = a0 & 0xFFFFFF;
                break;
            case RSPQ_CMD_CALL: {
                uint32_t target = a0 & 0xFFFFFF;
                bool placeholder = (int32_t)a[1] < 0;
                if (placeholder) {          // placeholder: target read from DMEM
                    target = *(uint32_t*)vrsp_dmem_addr(target) & 0xFFFFFF;
                    hle_rp_bad();
                }
                int slot = (a[1] & 0xFFC) >> 2;
                PSTACK(slot) = next;
                hle_rp_pstack(slot);
                // a recorded block starting from a state seen before: its output is copied
                if (placeholder || !hle_replay_call(target, slot))
                    next = target;
#ifdef HLE_TEST_PERF    // test aid: time spent inside recorded blocks
                {
                    extern uint32_t hle_blk_addr[512], hle_blk_mask, hle_blk_t0, hle_blk_ticks, hle_blk_calls; extern int hle_blk_n;
                    for (int i = 0; i < hle_blk_n; i++) if (hle_blk_addr[i] == target) {
                        if (!hle_blk_mask) hle_blk_t0 = TICKS_READ();
                        hle_blk_mask |= 1u << ((a[1] & 0xFFC) >> 2); hle_blk_calls++; break;
                    }
                }
#endif
                break;
            }
            case RSPQ_CMD_RET:
                next = PSTACK((a0 & 0xFFC) >> 2) & 0xFFFFFF;
                hle_replay_ret((a0 & 0xFFC) >> 2);
#ifdef HLE_TEST_PERF
                {
                    extern uint32_t hle_blk_mask, hle_blk_t0, hle_blk_ticks;
                    uint32_t bit = 1u << ((a0 & 0xFFC) >> 2);
                    if (hle_blk_mask & bit) { hle_blk_mask &= ~bit; if (!hle_blk_mask) hle_blk_ticks += TICKS_READ() - hle_blk_t0; }
                }
#endif
                break;
            case RSPQ_CMD_DMA: {
                hle_rp_bad();
                uint8_t *dmem = vrsp_dmem_addr(a[1]);
                void *rdram = hle_rdram(a0);
                uint32_t len = (a[2] & 0xFFF) + 1;
                if ((int32_t)a[3] < 0) memcpy(rdram, dmem, len);
                else memcpy(dmem, rdram, len);
                break;
            }
            case RSPQ_CMD_WRITE_STATUS:
                hle_rp_bad();
                vsp_write(a0 & 0x1FFFFFF);
                break;
            case RSPQ_CMD_SWAP_BUFFERS: {
                hle_rp_bad();
                vsp_write(a[2]);
                uint32_t target = PSTACK((a0 & 0xFFC) >> 2);
                PSTACK((a[1] & 0xFFC) >> 2) = next;
                next = target & 0xFFFFFF;
                break;
            }
            case RSPQ_CMD_TEST_WRITE_STATUS:
                // Signals are cleared synchronously, so the wait can never block.
                assertf(!(vsp_status & a[1]), "HLE: TEST_WRITE_STATUS would block");
                hle_rp_bad();
                vsp_write(a0 & 0x1FFFFFF);
                break;
            case RSPQ_CMD_RDP_SET_BUFFER:
                hle_rp_rdp_abs();
                vrsp_rdp_set_buffer(a0 & 0xFFFFFF, a[1], a[2]);
                break;
            case RSPQ_CMD_RDP_APPEND_BUFFER:
                hle_rp_rdp_abs();
                vrsp_rdp_append_buffer(a0 & 0xFFFFFF);
                break;
            }
            vrsp_ptr = next;
#ifdef HLE_TEST_PERF
            { extern uint32_t __hle_perf_cmd[16][64], __hle_perf_cnt[16][64]; __hle_perf_cmd[0][cmd_byte] += TICKS_READ() - c0; __hle_perf_cnt[0][cmd_byte]++; }
#endif
            continue;
        }

        uint8_t base = rspq_data.rspq_ovl_table.idmap[ovl_id] >> 2;
        if (__builtin_expect(vrsp_cur_ovl != base || !vrsp_img_loaded[base], 0)) {
            assertf(base, "HLE: command %08lx for unregistered overlay %ld", a0, ovl_id);
            vrsp_load_overlay(base);
        }
        uint32_t cmd = cmd_byte - (base << 4);
        int size = vrsp_cmdsize[base][cmd & 63];
        if (__builtin_expect(hle_rp_on, 0) && vrsp_handlers[base] != ovl_t3d && vrsp_handlers[base] != ovl_rdpq)
            hle_rp_bad_();      // only these two overlays' state is saved
        assertf(size || cmd == 0, "HLE: invalid command %02lx in overlay %s", cmd_byte, rspq_overlay_ucodes[base]->name);
        if (vrsp_handlers[base] == ovl_t3d && cmd == 0) {
            // Runs of Tiny3D triangles are the bulk of the queue: take them in one go
#ifdef HLE_TEST_PERF
            uint32_t c0 = TICKS_READ();
#endif
            vrsp_ptr += hle_t3d_tri_run(a, cmd_byte);
#ifdef HLE_TEST_PERF
            { extern uint32_t __hle_perf_cmd[16][64], __hle_perf_cnt[16][64]; __hle_perf_cmd[base][0] += TICKS_READ() - c0; __hle_perf_cnt[base][0]++; }
#endif
            continue;
        }
        vrsp_ptr += size;   // advance first: handlers may re-enter the queue state
#ifdef HLE_TEST_PERF
        uint32_t c0 = TICKS_READ();
#endif
        if (vrsp_handlers[base])
            vrsp_handlers[base](cmd, a, size / 4);
#ifdef HLE_TEST_PERF
        {
            extern uint32_t __hle_perf_cmd[16][64], __hle_perf_cnt[16][64];
            __hle_perf_cmd[base][cmd & 63] += TICKS_READ() - c0;
            __hle_perf_cnt[base][cmd & 63]++;
        }
#endif
    }

    vsp_status |= SP_STATUS_HALTED;
    vrsp_running = false;
#ifdef HLE_TEST_PERF
    __hle_perf_ticks += TICKS_READ() - t0;
#endif
}

// ---- Block replay: the virtual RSP's state ----

/** Tiny3D overlays with live state (rdpq's DMEM state only matters to SYNC_FULL, which ends a recording). */
uint32_t hle_vrsp_loaded_mask(void)
{
    uint32_t m = 0;
    for (int i = 1; i < RSPQ_MAX_OVERLAYS; i++)
        if (vrsp_img_loaded[i] && vrsp_handlers[i] == ovl_t3d) m |= 1u << i;     // rdpq's own DMEM: only SYNC_FULL reads it
#ifdef HLE_TEST_REPLAY_CHECK
    static bool shown;
    if (!shown && m) {
        shown = true;
        for (int i = 1; i < RSPQ_MAX_OVERLAYS; i++) if (m & (1u << i))
            debugf("[replay] overlay %d %s: state %03x +%lu\n", i, rspq_overlay_ucodes[i]->name, (unsigned)(vrsp_header(i)->state_start & 0xFFF), (unsigned long)vrsp_header(i)->state_size + 1);
    }
#endif
    return m;
}
void hle_vrsp_state(hle_st_t *s, uint32_t ovl_mask, bool sig)
{
    HLE_ST(s, rspq_data.rdp_mode); HLE_ST(s, rspq_data.rdp_scissor_rect);
    HLE_ST(s, rspq_data.rdp_fill_color); HLE_ST(s, rspq_data.rdp_target_bitdepth);
    for (int i = 1; i < RSPQ_MAX_OVERLAYS; i++) {
        if (!(ovl_mask & (1u << i))) continue;
        // (only Tiny3D overlays are in the mask) the DMEM the port reads, segment table aside
        uint32_t lo, hi; hle_t3d_dmem_range(&lo, &hi);
        uint8_t *at = s->p;
        if (s->load) hle_t3d_seg_keep(vrsp_imgs[i], false);
        hle_st(s, vrsp_imgs[i] + lo, hi - lo);
        if (s->load) hle_t3d_seg_keep(vrsp_imgs[i], true);
        else hle_t3d_seg_mask(at, lo);          // kept as it is on load
    }
    if (!sig) {
        HLE_ST(s, vrsp_cur_ovl);
        if (s->load && vrsp_cur_ovl) vrsp_load_overlay(vrsp_cur_ovl);
    }
}
const uint8_t *hle_vrsp_t3d_img(void)
{
    for (int i = 1; i < RSPQ_MAX_OVERLAYS; i++) if (vrsp_handlers[i] == ovl_t3d && vrsp_imgs[i]) return vrsp_imgs[i];
    return __vrsp_img;
}
uint32_t hle_vrsp_pstack_io(uint8_t *buf, int slot, bool load)
{
    if (load) memcpy(&PSTACK(slot), buf, 4); else memcpy(buf, &PSTACK(slot), 4);
    return 4;
}
void hle_vrsp_rdp_get(uint32_t *cur, uint32_t *sentinel, uint32_t *dp, uint8_t *syncfull)
{
    *cur = rspq_data.rspq_rdp_current; *sentinel = rspq_data.rspq_rdp_sentinel;
    *dp = hle_vrdp_dp_cur(); *syncfull = rspq_data.rdp_syncfull_ongoing;
}
void hle_vrsp_rdp_set(uint32_t cur, uint32_t sentinel, uint32_t dp, uint8_t syncfull)
{
    rspq_data.rspq_rdp_current = cur; rspq_data.rspq_rdp_sentinel = sentinel;
    hle_vrdp_dp_cur_set(dp); rspq_data.rdp_syncfull_ongoing = syncfull;
}
void hle_vrsp_rdp_advance(uint32_t nbytes)
{
    rspq_data.rspq_rdp_current += nbytes;
    hle_vrdp_dp_cur_set(rspq_data.rspq_rdp_current);
}
bool hle_vrsp_rdp_room(uint32_t nbytes)
{
    return !nbytes || (int32_t)(rspq_data.rspq_rdp_sentinel - nbytes) >= (int32_t)(rspq_data.rspq_rdp_current & 0xFFFFFF);
}
bool hle_vrsp_rdp_synced(void)
{
    return hle_vrdp_dp_cur() == (rspq_data.rspq_rdp_current & 0xFFFFFF);
}
