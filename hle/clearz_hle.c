/**
 * rdpq_clear_z without the RSP (linked with --wrap __rdpq_clear_z).
 *
 * libdragon clears the Z buffer with an RSP DMA, preceded by rdpq_fence(): a
 * SYNC_FULL in the middle of the frame. In this build every SYNC_FULL ends a
 * graphics task, so each frame reached the emulator as [clear to black] + [the
 * rest], and plugins that present per task (Delta) flashed black frames. This
 * fills the Z buffer with the RDP instead, retargeting the color image directly:
 * libdragon's own fallback attaches/detaches, and rdpq_detach is a SYNC_FULL too.
 */
#include <libdragon.h>

void __wrap___rdpq_clear_z(const uint16_t *z)
{
    const surface_t *zbuf = display_get_zbuf();     // the only Z buffer this game attaches
    uint32_t old_cfg = rdpq_config_disable(RDPQ_CFG_AUTOSCISSOR);   // keep the scissor, like rdpq_clear
    rdpq_set_color_image(zbuf);
    rdpq_mode_push();
        rdpq_set_mode_fill(color_from_packed16(z ? *z : ZBUF_MAX));
        rdpq_fill_rectangle(0, 0, zbuf->width, zbuf->height);
    rdpq_mode_pop();
    rdpq_set_color_image(rdpq_get_attached());
    rdpq_config_set(old_cfg);
}
