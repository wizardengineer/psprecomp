#pragma once
#include <cstdint>

/// Initialize the GE draw infrastructure: FBO, VAO/VBO, shader.
/// Must be called after SDL/GL init and after ge_init()/ge_texture_init().
void ge_draw_init();

/// Shutdown the GE draw infrastructure.
void ge_draw_shutdown();

/// Called at start of display list processing (bind FBO, etc.)
void ge_draw_begin_list();

/// Called at end of display list processing.
void ge_draw_end_list();

/// Full draw pipeline for a GE PRIM command:
/// decode vertices -> transform -> upload VBO -> set GL state -> draw.
void ge_draw_prim(
    uint8_t* rdram,
    int prim_type,
    int count
);

/// Present the FBO to the SDL2 window via glBlitFramebuffer.
/// Does NOT read rdram for pixel data (FBO blit only).
void ge_present_frame(
    uint8_t* rdram,
    uint32_t fb_addr,
    uint32_t fb_stride,
    uint32_t fb_format
);

/// Snapshot the GE draw counters (issue #35 debug socket I command).
/// Lock-free racy reads of the draw-thread counters -- values may be a
/// few prims stale, which is fine for diagnostics. Any pointer may be null.
void ge_draw_get_stats(
    uint32_t* frames,
    int* prims_total,
    int* real_nonsprite,
    int* sprite_nonclear,
    int* clears
);

/// On-demand screenshot (issue #35 debug socket S command).
/// Callable from ANY thread: posts a capture request and blocks until the
/// render thread services it (or timeout_ms elapses). Returns true once
/// the TGA file has been written. Never touches GL itself.
bool ge_draw_capture_screenshot(const char* path, int timeout_ms);

/// Render-thread side of ge_draw_capture_screenshot: if a capture request
/// is pending, read the FBO and write the TGA. MUST be called from the
/// main (GL) thread -- wired into psp_event_loop next to
/// render_queue_process().
void ge_draw_service_screenshot_request();
