#include "internal.h"
#include "x11.h"
#include "backend.h"

#include "session/udev.h"

#include "compositor/compositor.h"
#include "compositor/output.h"

#include <xcb/xcb.h>
#include <X11/Xlib-xcb.h>
#include <linux/input.h>

#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <assert.h>

#include <wayland-server.h>
#include <wayland-util.h>

// FIXME: contains global state

enum atom_name {
   WM_PROTOCOLS,
   WM_DELETE_WINDOW,
   WM_CLASS,
   NET_WM_NAME,
   STRING,
   UTF8_STRING,
   ATOM_LAST
};

static struct {
   Display *display;
   xcb_connection_t *connection;
   xcb_screen_t *screen;
   xcb_cursor_t cursor;
   xcb_atom_t atoms[ATOM_LAST];

   struct wlc_compositor *compositor;
   struct wl_event_source *event_source;

   struct {
      void *x11_handle;
      void *x11_xcb_handle;
      void *xcb_handle;

      Display* (*XOpenDisplay)(const char*);
      int (*XCloseDisplay)(Display*);

      xcb_connection_t* (*XGetXCBConnection)(Display*);
      void (*XSetEventQueueOwner)(Display*, enum XEventQueueOwner);

      int (*xcb_flush)(xcb_connection_t*);
      int (*xcb_connection_has_error)(xcb_connection_t*);
      const xcb_setup_t* (*xcb_get_setup)(xcb_connection_t*);
      xcb_screen_iterator_t (*xcb_setup_roots_iterator)(const xcb_setup_t*);
      uint32_t (*xcb_generate_id)(xcb_connection_t*);
      xcb_void_cookie_t (*xcb_create_window_checked)(xcb_connection_t*, uint8_t, xcb_window_t, xcb_window_t, int16_t, int16_t, uint16_t, uint16_t, uint16_t, uint16_t, xcb_visualid_t, uint32_t, const uint32_t*);
      xcb_void_cookie_t (*xcb_destroy_window)(xcb_connection_t*, xcb_window_t);
      xcb_void_cookie_t (*xcb_map_window_checked)(xcb_connection_t*, xcb_window_t);
      xcb_void_cookie_t (*xcb_create_pixmap)(xcb_connection_t*, uint8_t, xcb_pixmap_t, xcb_drawable_t, uint16_t, uint16_t);
      xcb_void_cookie_t (*xcb_create_gc)(xcb_connection_t*, xcb_gcontext_t, xcb_drawable_t, uint32_t, const uint32_t*);
      xcb_void_cookie_t (*xcb_free_pixmap)(xcb_connection_t*, xcb_pixmap_t pixmap);
      xcb_void_cookie_t (*xcb_free_gc)(xcb_connection_t*, xcb_gcontext_t);
      xcb_void_cookie_t (*xcb_put_image)(xcb_connection_t*, uint8_t, xcb_drawable_t, xcb_gcontext_t, uint16_t, uint16_t, int16_t, int16_t, uint8_t, uint8_t, uint32_t, const uint8_t*);
      xcb_void_cookie_t (*xcb_create_cursor)(xcb_connection_t*, xcb_cursor_t, xcb_pixmap_t, xcb_pixmap_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t);
      xcb_void_cookie_t (*xcb_free_cursor)(xcb_connection_t*, xcb_cursor_t);
      xcb_void_cookie_t (*xcb_change_property)(xcb_connection_t*, uint8_t, xcb_window_t, xcb_atom_t, xcb_atom_t, uint8_t, uint32_t, const void*);
      xcb_void_cookie_t (*xcb_change_window_attributes_checked)(xcb_connection_t*, xcb_window_t, uint32_t, const uint32_t*);
      xcb_intern_atom_cookie_t (*xcb_intern_atom)(xcb_connection_t*, uint8_t, uint16_t, const char*);
      xcb_intern_atom_reply_t* (*xcb_intern_atom_reply)(xcb_connection_t*, xcb_intern_atom_cookie_t, xcb_generic_error_t**);
      xcb_generic_error_t* (*xcb_request_check)(xcb_connection_t*, xcb_void_cookie_t);
      xcb_generic_event_t* (*xcb_poll_for_event)(xcb_connection_t*);
      int (*xcb_get_file_descriptor)(xcb_connection_t*);
   } api;
} x11;

static bool
x11_load(void)
{
   const char *lib = "libX11.so", *func = NULL;

   if (!(x11.api.x11_handle = dlopen(lib, RTLD_LAZY))) {
      wlc_log(WLC_LOG_WARN, "%s", dlerror());
      return false;
   }

#define load(x) (x11.api.x = dlsym(x11.api.x11_handle, (func = #x)))

   if (!load(XOpenDisplay))
      goto function_pointer_exception;
   if (!load(XCloseDisplay))
      goto function_pointer_exception;

#undef load

   return true;

function_pointer_exception:
   wlc_log(WLC_LOG_WARN, "Could not load function '%s' from '%s'", func, lib);
   return false;
}

static bool
x11_xcb_load(void)
{
   const char *lib = "libX11-xcb.so", *func = NULL;

   if (!(x11.api.x11_xcb_handle = dlopen(lib, RTLD_LAZY))) {
      wlc_log(WLC_LOG_WARN, "%s", dlerror());
      return false;
   }

#define load(x) (x11.api.x = dlsym(x11.api.x11_xcb_handle, (func = #x)))

   if (!load(XGetXCBConnection))
      goto function_pointer_exception;
   if (!load(XSetEventQueueOwner))
      goto function_pointer_exception;

#undef load

   return true;

function_pointer_exception:
   wlc_log(WLC_LOG_WARN, "Could not load function '%s' from '%s'", func, lib);
   return false;
}

static bool
xcb_load(void)
{
   const char *lib = "libxcb.so", *func = NULL;

   if (!(x11.api.xcb_handle = dlopen(lib, RTLD_LAZY))) {
      wlc_log(WLC_LOG_WARN, "%s", dlerror());
      return false;
   }

#define load(x) (x11.api.x = dlsym(x11.api.xcb_handle, (func = #x)))

   if (!load(xcb_flush))
      goto function_pointer_exception;
   if (!load(xcb_connection_has_error))
      goto function_pointer_exception;
   if (!load(xcb_get_setup))
      goto function_pointer_exception;
   if (!load(xcb_setup_roots_iterator))
      goto function_pointer_exception;
   if (!load(xcb_generate_id))
      goto function_pointer_exception;
   if (!load(xcb_create_window_checked))
      goto function_pointer_exception;
   if (!load(xcb_destroy_window))
      goto function_pointer_exception;
   if (!load(xcb_map_window_checked))
      goto function_pointer_exception;
   if (!load(xcb_create_pixmap))
      goto function_pointer_exception;
   if (!load(xcb_create_gc))
      goto function_pointer_exception;
   if (!load(xcb_free_pixmap))
      goto function_pointer_exception;
   if (!load(xcb_free_gc))
      goto function_pointer_exception;
   if (!load(xcb_put_image))
      goto function_pointer_exception;
   if (!load(xcb_create_cursor))
      goto function_pointer_exception;
   if (!load(xcb_free_cursor))
      goto function_pointer_exception;
   if (!load(xcb_change_property))
      goto function_pointer_exception;
   if (!load(xcb_change_window_attributes_checked))
      goto function_pointer_exception;
   if (!load(xcb_intern_atom))
      goto function_pointer_exception;
   if (!load(xcb_intern_atom_reply))
      goto function_pointer_exception;
   if (!load(xcb_request_check))
      goto function_pointer_exception;
   if (!load(xcb_poll_for_event))
      goto function_pointer_exception;
   if (!load(xcb_get_file_descriptor))
      goto function_pointer_exception;

#undef load

   return true;

function_pointer_exception:
   wlc_log(WLC_LOG_WARN, "Could not load function '%s' from '%s'", func, lib);
   return false;
}

static bool
page_flip(struct wlc_backend_surface *surface)
{
   struct wlc_output *output = surface->output;
   struct timespec ts;
   wlc_get_time(&ts);
   wlc_output_finish_frame(output, &ts);
   return true;
}

static void
surface_free(struct wlc_backend_surface *bsurface)
{
   x11.api.xcb_destroy_window(x11.connection, bsurface->window);
}

static bool
add_output(xcb_window_t window, struct wlc_output_information *info)
{
   struct wlc_backend_surface *bsurface = NULL;

   if (!(bsurface = wlc_backend_surface_new(surface_free, 0)))
      return false;

   bsurface->window = window;
   bsurface->display = x11.display;
   bsurface->api.page_flip = page_flip;

   struct wlc_output_event ev = { .add = { bsurface, info }, .type = WLC_OUTPUT_EVENT_ADD };
   wl_signal_emit(&wlc_system_signals()->output, &ev);
   return true;
}

static struct wlc_output*
output_for_window(xcb_window_t window, struct wl_list *outputs)
{
   struct wlc_output *output;
   wl_list_for_each(output, outputs, link) {
      if (output->bsurface && output->bsurface->window == window)
         return output;
   }
   return NULL;
}

static double
pointer_abs_x(void *internal, uint32_t width)
{
   xcb_motion_notify_event_t *xev = internal;
   return fmin(fmax(xev->event_x, 0), width);
}

static double
pointer_abs_y(void *internal, uint32_t height)
{
   xcb_motion_notify_event_t *xev = internal;
   return fmin(fmax(xev->event_y, 0), height);
}

static int
x11_event(int fd, uint32_t mask, void *data)
{
   (void)fd, (void)mask;

   int count = 0;
   xcb_generic_event_t *event;
   struct wlc_compositor *compositor = data;

   while ((event = x11.api.xcb_poll_for_event(x11.connection))) {
      switch (event->response_type & ~0x80) {
         case XCB_EXPOSE:
            {
               xcb_expose_event_t *ev = (xcb_expose_event_t*)event;
               struct wlc_output *output;
               if ((output = output_for_window(ev->window, &compositor->outputs)))
                  wlc_output_schedule_repaint(output);
            }
            break;

         case XCB_CLIENT_MESSAGE:
            {
               xcb_client_message_event_t *ev = (xcb_client_message_event_t*)event;
               if (ev->data.data32[0] == x11.atoms[WM_DELETE_WINDOW]) {
                  struct wlc_output *output;
                  if ((output = output_for_window(ev->window, &compositor->outputs))) {
                     if (wl_list_length(&compositor->outputs) <= 1) {
                        wlc_terminate();
                     } else {
                        wlc_output_terminate(output);
                     }
                     return 1;
                  }
               }
            }
            break;

         case XCB_CONFIGURE_NOTIFY:
            {
               xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t*)event;
               struct wlc_output *output;
               if ((output = output_for_window(ev->event, &compositor->outputs)))
                  wlc_output_set_resolution(output, &(struct wlc_size){ ev->width, ev->height }); // XXX: make a request?
            }
            break;

         case XCB_FOCUS_IN:
            {
               xcb_focus_in_event_t *ev = (xcb_focus_in_event_t*)event;
               struct wlc_output *output;
               if ((output = output_for_window(ev->event, &compositor->outputs))) {
                  struct wlc_output_event ev = { .active = { output }, .type = WLC_OUTPUT_EVENT_ACTIVE };
                  wl_signal_emit(&wlc_system_signals()->output, &ev);
               }
            }
            break;

         case XCB_MOTION_NOTIFY:
         {
            xcb_motion_notify_event_t *xev = (xcb_motion_notify_event_t*)event;
            struct wlc_input_event ev;
            ev.type = WLC_INPUT_EVENT_MOTION_ABSOLUTE;
            ev.time = xev->time;
            ev.motion_abs.x = pointer_abs_x;
            ev.motion_abs.y = pointer_abs_y;
            ev.motion_abs.internal = xev;
            wl_signal_emit(&wlc_system_signals()->input, &ev);
         }
         break;

         case XCB_BUTTON_PRESS:
         {
            xcb_button_press_event_t *xev = (xcb_button_press_event_t*)event;
            struct wlc_input_event ev;
            ev.type = WLC_INPUT_EVENT_BUTTON;
            ev.time = xev->time;
            ev.button.code = (xev->detail == 2 ? BTN_MIDDLE : (xev->detail == 3 ? BTN_RIGHT : xev->detail + BTN_LEFT - 1));
            ev.button.state = WL_POINTER_BUTTON_STATE_PRESSED;
            wl_signal_emit(&wlc_system_signals()->input, &ev);
         }
         break;

         case XCB_BUTTON_RELEASE:
         {
            xcb_button_press_event_t *xev = (xcb_button_press_event_t*)event;
            struct wlc_input_event ev;
            ev.time = xev->time;
            switch (xev->detail) {
               case 4:
               case 5:
                  ev.type = WLC_INPUT_EVENT_SCROLL;
                  ev.scroll.axis_bits = WLC_SCROLL_AXIS_VERTICAL;
                  ev.scroll.amount[0] = (xev->detail == 4 ? -10 : 10);
                  break;

               case 6:
               case 7:
                  ev.type = WLC_INPUT_EVENT_SCROLL;
                  ev.scroll.axis_bits = WLC_SCROLL_AXIS_HORIZONTAL;
                  ev.scroll.amount[1] = (xev->detail == 6 ? -10 : 10);
                  break;

               default:
                  ev.type = WLC_INPUT_EVENT_BUTTON;
                  ev.time = xev->time;
                  ev.button.code = (xev->detail == 2 ? BTN_MIDDLE : (xev->detail == 3 ? BTN_RIGHT : xev->detail + BTN_LEFT - 1));
                  ev.button.state = WL_POINTER_BUTTON_STATE_RELEASED;
                  break;
            }
            wl_signal_emit(&wlc_system_signals()->input, &ev);
         }
         break;

         case XCB_KEY_PRESS:
         {
            xcb_key_press_event_t *xev = (xcb_key_press_event_t*)event;
            struct wlc_input_event ev;
            ev.type = WLC_INPUT_EVENT_KEY;
            ev.time = xev->time;
            ev.key.code = xev->detail - 8;
            ev.key.state = WL_KEYBOARD_KEY_STATE_PRESSED;
            wl_signal_emit(&wlc_system_signals()->input, &ev);
         }
         break;

         case XCB_KEY_RELEASE:
         {
            xcb_key_press_event_t *xev = (xcb_key_press_event_t*)event;
            struct wlc_input_event ev;
            ev.type = WLC_INPUT_EVENT_KEY;
            ev.time = xev->time;
            ev.key.code = xev->detail - 8;
            ev.key.state = WL_KEYBOARD_KEY_STATE_RELEASED;
            wl_signal_emit(&wlc_system_signals()->input, &ev);
         }
         break;
      }

      free(event);
      count += 1;
   }

   x11.api.xcb_flush(x11.connection);
   return count;
}

static uint32_t
update_outputs(struct wl_list *outputs)
{
   uint32_t alive = 0;
   if (outputs) {
      struct wlc_output *o;
      wl_list_for_each(o, outputs, link) {
         if (o->bsurface)
            ++alive;
      }
   }

   const char *env;
   uint32_t fakes = 1;
   if ((env = getenv("WLC_OUTPUTS")))
      fakes = fmax(strtol(env, NULL, 10), 1);

   if (alive >= fakes)
      return 0;

   struct wlc_output_information info;
   memset(&info, 0, sizeof(info));
   wlc_string_set(&info.make, "Xorg", false);
   wlc_string_set(&info.model, "X11 Window", false);
   info.scale = 1;

   struct wlc_output_mode mode;
   memset(&mode, 0, sizeof(mode));
   mode.refresh = 60;
   mode.width = x11.screen->width_in_pixels;
   mode.height = x11.screen->height_in_pixels;
   mode.flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
   wlc_output_information_add_mode(&info, &mode);

   uint32_t count = 0;

   xcb_generic_error_t *error;
   unsigned int root_mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS;
   if ((error = x11.api.xcb_request_check(x11.connection, x11.api.xcb_change_window_attributes_checked(x11.connection, x11.screen->root, XCB_CW_EVENT_MASK, &root_mask)))) {
      uint32_t mask = XCB_CW_EVENT_MASK | XCB_CW_CURSOR;
      uint32_t values[] = {
         XCB_EVENT_MASK_FOCUS_CHANGE |
         XCB_EVENT_MASK_EXPOSURE |
         XCB_EVENT_MASK_STRUCTURE_NOTIFY |
         XCB_EVENT_MASK_POINTER_MOTION |
         XCB_EVENT_MASK_BUTTON_PRESS |
         XCB_EVENT_MASK_BUTTON_RELEASE |
         XCB_EVENT_MASK_KEY_PRESS |
         XCB_EVENT_MASK_KEY_RELEASE,
         x11.cursor,
      };

      for (uint32_t i = 0; i < fakes; ++i) {
         xcb_window_t window;
         if (!(window = x11.api.xcb_generate_id(x11.connection)))
            continue;

         xcb_void_cookie_t create_cookie = x11.api.xcb_create_window_checked(x11.connection, XCB_COPY_FROM_PARENT, window, x11.screen->root, 0, 0, 800, 480, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, x11.screen->root_visual, mask, values);
         x11.api.xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, window, x11.atoms[WM_PROTOCOLS], XCB_ATOM_ATOM, 32, 1, &x11.atoms[WM_DELETE_WINDOW]);
         x11.api.xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, window, x11.atoms[NET_WM_NAME], x11.atoms[UTF8_STRING], 8, 7, "wlc-x11");
         x11.api.xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, window, x11.atoms[WM_CLASS], x11.atoms[STRING], 8, 7, "wlc-x11");
         xcb_void_cookie_t map_cookie = x11.api.xcb_map_window_checked(x11.connection, window);

         if ((error = x11.api.xcb_request_check(x11.connection, create_cookie)) || (error = x11.api.xcb_request_check(x11.connection, map_cookie))) {
            free(error);
            continue;
         }

         count += (add_output(window, &info) ? 1 : 0);

         if (i < fakes) {
            struct wl_array cpy;
            wl_array_init(&cpy);
            wl_array_copy(&cpy, &info.modes);
            memcpy(&info.modes, &cpy, sizeof(struct wl_array));
         }
      }
   } else {
      count += (add_output(x11.screen->root, &info) ? 1 : 0);
   }

   return count;
}

static void
terminate(void)
{
   if (x11.cursor)
      x11.api.xcb_free_cursor(x11.connection, x11.cursor);

   if (x11.display)
      x11.api.XCloseDisplay(x11.display);

   if (x11.event_source)
      wl_event_source_remove(x11.event_source);

   if (x11.api.x11_handle)
      dlclose(x11.api.x11_handle);

   if (x11.api.x11_xcb_handle)
      dlclose(x11.api.x11_xcb_handle);

   if (x11.api.xcb_handle)
      dlclose(x11.api.xcb_handle);

   memset(&x11, 0, sizeof(x11));
}

bool
wlc_x11_init(struct wlc_backend *out_backend, struct wlc_compositor *compositor)
{
   (void)compositor;

   if (!x11_load() || !x11_xcb_load() || !xcb_load())
      goto fail;

   if (!(x11.display = x11.api.XOpenDisplay(NULL)))
      goto display_open_fail;

   if (!(x11.connection = x11.api.XGetXCBConnection(x11.display)))
      goto xcb_connection_fail;

   x11.api.XSetEventQueueOwner(x11.display, XCBOwnsEventQueue);

   if (x11.api.xcb_connection_has_error(x11.connection))
      goto xcb_connection_fail;

   struct {
      const char *name;
      enum atom_name atom;
   } map[ATOM_LAST] = {
      { "WM_PROTOCOLS", WM_PROTOCOLS },
      { "WM_DELETE_WINDOW", WM_DELETE_WINDOW },
      { "WM_CLASS", WM_CLASS },
      { "NET_WM_NAME", NET_WM_NAME },
      { "STRING", STRING },
      { "UTF8_STRING", UTF8_STRING },
   };

   for (int i = 0; i < ATOM_LAST; ++i) {
      xcb_intern_atom_reply_t *reply = x11.api.xcb_intern_atom_reply(x11.connection, x11.api.xcb_intern_atom(x11.connection, 0, strlen(map[i].name), map[i].name), 0);
      x11.atoms[map[i].atom] = (reply ? reply->atom : 0);
   }

   xcb_screen_iterator_t s = x11.api.xcb_setup_roots_iterator(x11.api.xcb_get_setup(x11.connection));
   x11.screen = s.data;

   xcb_gc_t gc = x11.api.xcb_generate_id(x11.connection);
   xcb_pixmap_t pixmap = x11.api.xcb_generate_id(x11.connection);

   if (!(x11.cursor = x11.api.xcb_generate_id(x11.connection)))
      goto cursor_fail;

   uint8_t data[] = { 0, 0, 0, 0 };
   x11.api.xcb_create_pixmap(x11.connection, 1, pixmap, x11.screen->root, 1, 1);
   x11.api.xcb_create_gc(x11.connection, gc, pixmap, 0, NULL);
   x11.api.xcb_put_image(x11.connection, XCB_IMAGE_FORMAT_XY_PIXMAP, pixmap, gc, 1, 1, 0, 0, 0, 32, sizeof(data), data);
   x11.api.xcb_create_cursor(x11.connection, x11.cursor, pixmap, pixmap, 0, 0, 0, 0, 0, 0, 1, 1);
   x11.api.xcb_free_gc(x11.connection, gc);
   x11.api.xcb_free_pixmap(x11.connection, pixmap);

   if (!update_outputs(NULL))
      goto output_fail;

   if (!wlc_input_has_init()) {
      if (!(x11.event_source = wl_event_loop_add_fd(wlc_event_loop(), x11.api.xcb_get_file_descriptor(x11.connection), WL_EVENT_READABLE, x11_event, compositor)))
         goto event_source_fail;

      wl_event_source_check(x11.event_source);
   }

   out_backend->api.update_outputs = update_outputs;
   out_backend->api.terminate = terminate;
   return true;

display_open_fail:
   wlc_log(WLC_LOG_WARN, "Failed to open X11 display");
   goto fail;
xcb_connection_fail:
   wlc_log(WLC_LOG_WARN, "Failed to get xcb connection");
   goto fail;
cursor_fail:
   wlc_log(WLC_LOG_WARN, "Failed to create empty X11 cursor");
   goto fail;
event_source_fail:
   wlc_log(WLC_LOG_WARN, "Failed to add X11 event source");
   goto fail;
output_fail:
   wlc_log(WLC_LOG_WARN, "Failed to create output");
fail:
   terminate();
   return false;
}
