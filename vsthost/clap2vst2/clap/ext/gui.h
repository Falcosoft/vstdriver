#pragma once
#include "../plugin.h"
static CLAP_CONSTEXPR const char CLAP_EXT_GUI[] = "clap.gui";
static const CLAP_CONSTEXPR char CLAP_WINDOW_API_WIN32[] = "win32";
static const CLAP_CONSTEXPR char CLAP_WINDOW_API_COCOA[] = "cocoa";
static const CLAP_CONSTEXPR char CLAP_WINDOW_API_X11[] = "x11";
static const CLAP_CONSTEXPR char CLAP_WINDOW_API_WAYLAND[] = "wayland";
#ifdef __cplusplus
extern "C" {
#endif
typedef void *clap_hwnd;
typedef void *clap_nsview;
typedef unsigned long clap_xwnd;
typedef struct clap_window {
   const char *api;
   union { clap_nsview cocoa; clap_xwnd x11; clap_hwnd win32; void *ptr; };
} clap_window_t;
typedef struct clap_gui_resize_hints {
   bool can_resize_horizontally;
   bool can_resize_vertically;
   bool preserve_aspect_ratio;
   uint32_t aspect_ratio_width;
   uint32_t aspect_ratio_height;
} clap_gui_resize_hints_t;
typedef struct clap_plugin_gui {
   bool(CLAP_ABI *is_api_supported)(const clap_plugin_t *plugin, const char *api, bool is_floating);
   bool(CLAP_ABI *get_preferred_api)(const clap_plugin_t *plugin, const char **api, bool *is_floating);
   bool(CLAP_ABI *create)(const clap_plugin_t *plugin, const char *api, bool is_floating);
   void(CLAP_ABI *destroy)(const clap_plugin_t *plugin);
   bool(CLAP_ABI *set_scale)(const clap_plugin_t *plugin, double scale);
   bool(CLAP_ABI *get_size)(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height);
   bool(CLAP_ABI *can_resize)(const clap_plugin_t *plugin);
   bool(CLAP_ABI *get_resize_hints)(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints);
   bool(CLAP_ABI *adjust_size)(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height);
   bool(CLAP_ABI *set_size)(const clap_plugin_t *plugin, uint32_t width, uint32_t height);
   bool(CLAP_ABI *set_parent)(const clap_plugin_t *plugin, const clap_window_t *window);
   bool(CLAP_ABI *set_transient)(const clap_plugin_t *plugin, const clap_window_t *window);
   void(CLAP_ABI *suggest_title)(const clap_plugin_t *plugin, const char *title);
   bool(CLAP_ABI *show)(const clap_plugin_t *plugin);
   bool(CLAP_ABI *hide)(const clap_plugin_t *plugin);
} clap_plugin_gui_t;
typedef struct clap_host_gui {
   void(CLAP_ABI *resize_hints_changed)(const clap_host_t *host);
   bool(CLAP_ABI *request_resize)(const clap_host_t *host, uint32_t width, uint32_t height);
   bool(CLAP_ABI *request_show)(const clap_host_t *host);
   bool(CLAP_ABI *request_hide)(const clap_host_t *host);
   void(CLAP_ABI *closed)(const clap_host_t *host, bool was_destroyed);
} clap_host_gui_t;
#ifdef __cplusplus
}
#endif
