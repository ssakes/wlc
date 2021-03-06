#ifndef _WLC_BACKEND_H_
#define _WLC_BACKEND_H_

#include "EGL/egl.h"
#include <stdbool.h>

struct wlc_compositor;
struct wlc_output;

struct wlc_backend_surface {
   void *internal;
   struct wlc_output *output;
   EGLNativeDisplayType display;
   EGLNativeWindowType window;
   size_t internal_size;

   struct {
      void (*terminate)(struct wlc_backend_surface *surface);
      void (*sleep)(struct wlc_backend_surface *surface, bool sleep);
      bool (*page_flip)(struct wlc_backend_surface *surface);
   } api;
};

struct wlc_backend {
   struct {
      uint32_t (*update_outputs)(struct wl_list *outputs);
      void (*terminate)(void);
   } api;
};

struct wlc_backend_surface* wlc_backend_surface_new(void (*destructor)(struct wlc_backend_surface*), size_t internal_size);
void wlc_backend_surface_free(struct wlc_backend_surface *surface);

uint32_t wlc_backend_update_outputs(struct wlc_backend *backend, struct wl_list *outputs);
void wlc_backend_terminate(struct wlc_backend *backend);
struct wlc_backend* wlc_backend_init(struct wlc_compositor *compositor);

#endif /* _WLC_BACKEND_H_ */
