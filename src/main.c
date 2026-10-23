#include <libdragon.h>
#include <string.h>
#include <t3d/t3d.h>
#include <t3d/t3ddebug.h>

#include "globals.h"
#include "game_time.h"
#include "joypad_utility.h"
#include "camera_controller.h"
#include "audio_controller.h"
#include "display_utility.h"
#include "controllers/menu_controller.h"
#include "save_controller.h"
#include "collision_system.h"
#include "scenes/guardian/guardian_scene.h"
#include "scene_controller.h"
#include "dev.h"
#include "dev/crt_safe_area_overlay.h"
#include "character/character.h"
#include "gpu_frame.h"

rspq_queue_t *g_drawQueue;
#ifdef HLE_LOG_FPS   // test aid: CPU ms of the main loop's update parts
static uint32_t s_mp[4];
#define MPROF(i, x) do { uint32_t _t = TICKS_READ(); x; s_mp[i] += TICKS_READ() - _t; } while (0)
#else
#define MPROF(i, x) x
#endif

int main(void)
{
#if defined(HLE_BUILD) || defined(HLE_AUTOPLAY)
    debug_init_isviewer();
    debugf("[hle] main\n");
#endif
    if (DEV_MODE)
    {
        dev_tools_init();
    }

    if (DEBUG_DRAW)
    {
        debugDraw = true;
    }
    else
    {
        debugDraw = false;
    }

    // INIT
    asset_init_compression(2);
    dfs_init(DFS_DEFAULT_LOCATION);

    // Safe: in case something left it open (some emus / hot reload flows)
    display_close();

    if (DITHER_ENABLED) {
        display_init(RESOLUTION_320x240, DEPTH_16_BPP, FRAME_BUFFER_COUNT, GAMMA_NONE, FILTERS_RESAMPLE_ANTIALIAS);
    } else {
        if (ARES_AA_ENABLED) {
            display_init(RESOLUTION_320x240, DEPTH_32_BPP, FRAME_BUFFER_COUNT, GAMMA_NONE, FILTERS_RESAMPLE_ANTIALIAS);
        } else {
            display_init(RESOLUTION_320x240, DEPTH_32_BPP, FRAME_BUFFER_COUNT, GAMMA_NONE, FILTERS_RESAMPLE);
        }
    }

    rdpq_init();

    audio_initialize();

    /*
     * Fonts registered once after final rdpq_init.
     * Opening credits now run as a normal scene, so they do not close/reinit display.
     */
    rdpq_text_register_font(
        FONT_BUILTIN_DEBUG_MONO,
        rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO)
    );

    rdpq_font_t *font1 = rdpq_font_load("rom:/fonts/unbalanced.font64");
    if (!font1) {
        debugf("[FATAL] failed to load rom:/fonts/unbalanced.font64\n");
        for (;;) {}
    }

    rdpq_text_register_font(FONT_UNBALANCED, font1);
    rdpq_font_style(font1, FONT_STYLE_GOLD, &(rdpq_fontstyle_t){ .color = RGBA32(255, 200, 90, 255) });
    rdpq_font_style(font1, FONT_STYLE_GREY, &(rdpq_fontstyle_t){ .color = RGBA32(130, 130, 130, 255) });
    rdpq_font_style(font1, FONT_STYLE_BLUE, &(rdpq_fontstyle_t){ .color = RGBA32(140, 185, 255, 255) });
    rdpq_font_style(font1, FONT_STYLE_RED, &(rdpq_fontstyle_t){ .color = RGBA32(235, 110, 85, 255) });

    game_time_init();
    joypad_utility_init();

    save_controller_init();
    (void)save_controller_load_settings();

    t3d_init((T3DInitParams){});
    T3DViewport viewport = t3d_viewport_create_buffered(GPU_FRAMES + 1);   // its matrices: the RSP may still be on the last frame

    if (DEV_MODE) {
        t3d_debug_print_init();
        dev_models_init();
    }

    /*
     * Transitional character ownership:
     * Character is initialized once at boot and is shared by title + Guardian.
     * Title and Guardian may position/reset/state-switch it, but neither scene
     * should call character_init() or character_free() during scene transitions.
     */
    character_init();

    /*
     * Menu controller is shared by title and Guardian.
     * Initialize it before scene_controller_init(), because title_scene_enter()
     * can immediately use title menu state.
     */
    menu_controller_init();

    /*
     * scene_controller now starts opening credits first unless skipped.
     * Flow:
     *   Opening Credits -> Title -> Guardian
     */
    scene_controller_init();

    if (DEV_MODE && debugDraw) {
        offscreenBuffer = surface_alloc(FMT_RGBA16, SCREEN_WIDTH, SCREEN_HEIGHT);

        if (!offscreenBuffer.buffer) {
            debugf("[FATAL] failed to allocate offscreenBuffer\n");
            for (;;) {}
        }
    }

    for (uint64_t frame = 0;; ++frame)
    {
#ifdef HLE_LOG_FPS
        uint32_t pt0 = TICKS_READ(), pt1 = pt0, pt2 = pt0;   // test aid: where a frame goes
#endif
        // The frame before last drawn: the RSP may still be on the last one (see the draw loop)
        static rspq_queue_t *drawQueue[2];
        static rspq_syncpoint_t drawDone[2];
        static int dq;
        dq ^= 1;
        if (drawDone[dq]) rspq_syncpoint_wait(drawDone[dq]);
        gpu_frame_begin(dq);
#ifdef HLE_LOG_FPS
        uint32_t ptGpu = TICKS_READ();
#endif

        // Update time + input first.
        game_time_update();
        joypad_update();

        // Debounced EEPROM save flush.
        save_controller_update();

        SceneControllerSceneId activeScene = scene_controller_get_active_scene();
        bool isOpeningCredits = activeScene == SCENE_CONTROLLER_SCENE_OPENING_CREDITS;
        bool isTitle = activeScene == SCENE_CONTROLLER_SCENE_TITLE;
        bool isGuardian = activeScene == SCENE_CONTROLLER_SCENE_GUARDIAN;

#ifdef HLE_LOG_FPS
        pt1 = TICKS_READ();
#endif

        // ===== UPDATE LOOP =====
#ifndef HLE_NO_MIXER   // test aid: the audio mixer's cost (silent)
        MPROF(0, mixer_try_play());
#endif

        if (DEV_MODE) {
            dev_controller_update();
        }

        bool devMenuOpen = DEV_MODE && dev_menu_is_open();
        bool cameraNeedsUpdate = (cameraState == CAMERA_FREECAM);

        if (!devMenuOpen)
        {
            /*
             * Opening credits are a real scene now, but they do not need camera,
             * menu, character, or fixed-update ownership.
             */
            if (!isOpeningCredits) {
                MPROF(1, camera_update(&viewport));
            }

            /*
             * Title menu input is updated inside title_scene_update()
             * via menu_controller_update_title().
             *
             * Normal menu_controller_update() still asks scene.c for GameState,
             * so only call it while Guardian is active.
             */
            if (isGuardian) {
                MPROF(2, menu_controller_update());
            }

            MPROF(3, scene_controller_update());

            /*
             * Fixed update currently belongs to Guardian only.
             * Title and Opening Credits do not need it.
             */
            if (isGuardian) {
                scene_fixed_update();
            }
        }
        else
        {
            if (cameraNeedsUpdate && !isOpeningCredits) {
                camera_update(&viewport);
            }

            if (isGuardian) {
                menu_controller_update();
            }
        }

        // Refresh active scene after update in case the scene controller switched scenes.
        activeScene = scene_controller_get_active_scene();
        isOpeningCredits = activeScene == SCENE_CONTROLLER_SCENE_OPENING_CREDITS;
        isTitle = activeScene == SCENE_CONTROLLER_SCENE_TITLE;
        isGuardian = activeScene == SCENE_CONTROLLER_SCENE_GUARDIAN;

#ifdef HLE_LOG_FPS
        pt2 = TICKS_READ();
#endif
        // Attach render target for the frame: after the update, so the CPU updates while it waits
        // for one to come free (the RSP draws the last frame meanwhile)
        if (DEV_MODE && debugDraw) {
            rdpq_attach(&offscreenBuffer, display_get_zbuf());
        } else {
            rdpq_attach(display_get(), display_get_zbuf());
        }
#ifdef HLE_LOG_FPS
        uint32_t ptFb = TICKS_READ();
#endif
        // ===== DRAW LOOP =====
        // Recorded, then run as a whole: submitting straight to the RSP stalled the CPU on its
        // small command ring, and the RSP then idled through the next frame's update. Now the RSP
        // draws this frame while the CPU updates the next (what it reads is buffered: kGpuFrames)
        if (!drawQueue[0]) { drawQueue[0] = rspq_queue_create(); drawQueue[1] = rspq_queue_create(); }
        rspq_queue_clear(drawQueue[dq]);
#if !defined(HLE_PROF_SERIAL) && !defined(HLE_PROF_BOSS) && !defined(HLE_PROF_DRAGON)    // test aids that time the RSP stage by stage
        g_drawQueue = drawQueue[dq];
        rspq_queue_switch(g_drawQueue);
#endif
        if (!devMenuOpen || cameraNeedsUpdate || isOpeningCredits) {
            scene_controller_draw(&viewport);
#ifdef HLE_PROF_SERIAL
            if (g_drawQueue) gpu_prof_end();    // a stage left open (an early return)
#endif
        }

        /*
         * Title and Guardian use Tiny3D. Opening Credits are plain RDPQ sprites,
         * so avoid forcing Tiny3D sync for that scene.
         */
        if (!isOpeningCredits) {
            t3d_tri_sync();
        }

        rdpq_sync_pipe();

        /*
         * Shared menu draw.
         * On title, title_scene_update() updates title menu state.
         * On Guardian, main updates pause menu state above.
         * Opening Credits should not draw the menu.
         */
        if (!isOpeningCredits) {
            menu_controller_draw();
        }

        if (DEV_MODE)
        {
            /*
             * Dev arrow/collision drawing are Tiny3D/game-world debug tools.
             * Do not draw them over opening credits.
             */
            if (!isOpeningCredits) {
                dev_draw_update(&viewport);
            }

            dev_update();

            if (debugDraw && isGuardian) {
                collision_draw(&viewport);
            }
        }

        if (SHOW_FPS)
        {
            rdpq_sync_pipe();
            rdpq_text_printf(
                NULL,
                FONT_BUILTIN_DEBUG_MONO,
                250,
                225,
                " %.2f",
                display_get_fps()
            );
        }

        rspq_queue_switch(NULL);
        g_drawQueue = NULL;
        rspq_queue_run(drawQueue[dq]);
        drawDone[dq] = rspq_syncpoint_new();
#ifdef HLE_PROF_WAITEND   // test aid: the frame's GPU time with the CPU idle
        { static uint32_t acc, n; rspq_flush(); uint32_t t = TICKS_READ();
#if HLE_PROF_WAITEND == 2       // the CPU busy 12 ms on arithmetic alone
          { volatile uint32_t x = 1; while (TICKS_READ() - t < TICKS_FROM_MS(12)) for (int i = 0; i < 200; i++) x = x * 1664525u + 1013904223u; }
#elif HLE_PROF_WAITEND == 3     // writing uncached memory
          { static uint32_t *u; if (!u) u = malloc_uncached(16384); while (TICKS_READ() - t < TICKS_FROM_MS(12)) for (int i = 0; i < 4096; i++) u[i] = i; }
#elif HLE_PROF_WAITEND == 5     // reading uncached memory
          { static uint32_t *u; if (!u) u = malloc_uncached(16384); volatile uint32_t x = 0; while (TICKS_READ() - t < TICKS_FROM_MS(12)) for (int i = 0; i < 4096; i++) x += u[i]; }
#elif HLE_PROF_WAITEND == 4     // missing the cache (reading 256 KB round and round)
          { static uint32_t *c; if (!c) c = malloc(65536 * 4); volatile uint32_t x = 0; while (TICKS_READ() - t < TICKS_FROM_MS(12)) for (int i = 0; i < 65536; i += 4) x += c[i]; }
#endif
          rspq_wait(); acc += TICKS_READ() - t;
          if (++n == 120) { debugf("[gpuframe] %.2f ms\n", TICKS_TO_US(acc / n) / 1000.0); acc = n = 0; } }
#endif

        if (DEV_MODE && debugDraw)
        {
            rdpq_detach();

            rdpq_attach(display_get(), display_get_zbuf());
            rdpq_set_mode_standard();
            rdpq_tex_blit(&offscreenBuffer, 0, 0, NULL);

            if (debugDraw && DRAW_CRT_SAFE_AREA) {
                DrawCrtSafeAreaOverlay(display_get_width(), display_get_height());
            }

            rdpq_detach_show();
        }
        else
        {
            if (debugDraw && DRAW_CRT_SAFE_AREA) {
                DrawCrtSafeAreaOverlay(display_get_width(), display_get_height());
            }

            rdpq_detach_show();
        }
        // The RSP halts when it runs out of commands and wakes only on a flush: without one it
        // could sleep through this frame's draw until the next frame waited on it
        rspq_flush();

        if (DEV_MODE)
        {
            dev_frame_update();
        }
#ifdef HLE_LOG_FPS
        {   // ms per frame: blocked for a free framebuffer (the GPU is behind), update, draw submission
            static uint32_t gwait, wait, upd, drw, n, lastStart, worst, slow;
            uint32_t pt3 = TICKS_READ();
            gwait += ptGpu - pt0; wait += ptFb - pt2; upd += pt2 - pt1; drw += pt3 - ptFb;
            extern int deltaFrames;
            if (lastStart) { uint32_t ft = pt0 - lastStart; if (ft > worst) worst = ft; }
            slow += deltaFrames - 1;    // refreshes that showed a frame again
            lastStart = pt0;
            if (n == 119) { int pl = 0; for (int ch = 0; ch < 16; ch++) pl += mixer_ch_playing(ch); debugf("[mixer] %d channels playing\n", pl); }
            if (n == 119) { debugf("[mainupd] mixer %.2f camera %.2f menu %.2f scene %.2f ms\n", TICKS_TO_US(s_mp[0] / 120) / 1000.0, TICKS_TO_US(s_mp[1] / 120) / 1000.0, TICKS_TO_US(s_mp[2] / 120) / 1000.0, TICKS_TO_US(s_mp[3] / 120) / 1000.0); memset(s_mp, 0, sizeof s_mp); }
            if (++n == 120) { debugf("[frame] wait for the RSP %.1f, a framebuffer %.1f, update %.1f draw %.1f ms; worst %.1f ms, %lu refreshes missed\n", TICKS_TO_US(gwait / n) / 1000.0, TICKS_TO_US(wait / n) / 1000.0, TICKS_TO_US(upd / n) / 1000.0, TICKS_TO_US(drw / n) / 1000.0,
                                      TICKS_TO_US(worst) / 1000.0, (unsigned long)slow); gwait = wait = upd = drw = n = worst = slow = 0; }
        }
#endif

        if (frame >= 30)
        {
            if (DEV_MODE) {
                dev_frames_end_update();
            }

            frame = 0;
        }

        (void)isTitle;
    }

    // Unreachable in normal runtime.
    scene_cleanup();
    character_free();
    menu_controller_free();
    save_controller_free();

    return 0;
}