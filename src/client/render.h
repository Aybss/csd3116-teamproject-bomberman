/* render.h - everything the client draws with OpenGL.
 * Uses pixel coordinates (0,0 = top-left, 800x600). */
#ifndef BA_RENDER_H
#define BA_RENDER_H

#include "client_net.h"

/* Call once after the GL context exists. */
void render_init(void);

/* Draw one frame. now_ns is the monotonic clock, dt_s is the time since the
   last frame (for animations). */
void render_frame(const ba_client_view *v, int64_t now_ns, float dt_s);

#endif /* BA_RENDER_H */
