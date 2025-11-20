#include <stdbool.h>

#include "../gfx_widgets.h"

static bool gfx_widget_netplay_init(
      gfx_display_t *p_disp,
      gfx_animation_t *p_anim,
      bool video_is_threaded, bool fullscreen)
{
   (void)p_disp;
   (void)p_anim;
   (void)video_is_threaded;
   (void)fullscreen;
   return true;
}

static void gfx_widget_netplay_free(void)
{
}

static void gfx_widget_netplay_context_reset(bool is_threaded,
      unsigned width, unsigned height, bool fullscreen,
      const char *dir_assets, char *font_path,
      char* menu_png_path, char* widgets_png_path)
{
   (void)is_threaded;
   (void)width;
   (void)height;
   (void)fullscreen;
   (void)dir_assets;
   (void)font_path;
   (void)menu_png_path;
   (void)widgets_png_path;
}

static void gfx_widget_netplay_context_destroy(void)
{
}

static void gfx_widget_netplay_layout(void *data,
      bool is_threaded, const char *dir_assets, char *font_path)
{
   (void)data;
   (void)is_threaded;
   (void)dir_assets;
   (void)font_path;
}

static void gfx_widget_netplay_iterate(void *user_data,
      unsigned width, unsigned height, bool fullscreen,
      const char *dir_assets, char *font_path,
      bool is_threaded)
{
   (void)user_data;
   (void)width;
   (void)height;
   (void)fullscreen;
   (void)dir_assets;
   (void)font_path;
   (void)is_threaded;
}

static void gfx_widget_netplay_frame(void* data, void *userdata)
{
   (void)data;
   (void)userdata;
}

const gfx_widget_t gfx_widget_netplay_chat = {
   gfx_widget_netplay_init,
   gfx_widget_netplay_free,
   gfx_widget_netplay_context_reset,
   gfx_widget_netplay_context_destroy,
   gfx_widget_netplay_layout,
   gfx_widget_netplay_iterate,
   gfx_widget_netplay_frame
};

const gfx_widget_t gfx_widget_netplay_ping = {
   gfx_widget_netplay_init,
   gfx_widget_netplay_free,
   gfx_widget_netplay_context_reset,
   gfx_widget_netplay_context_destroy,
   gfx_widget_netplay_layout,
   gfx_widget_netplay_iterate,
   gfx_widget_netplay_frame
};
