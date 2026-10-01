
/**
 * @file main
 *
 */

/*********************
 *      INCLUDES
 *********************/
#define _DEFAULT_SOURCE /* needed for usleep() */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include "lvgl/lvgl.h"
#include "lvgl/examples/lv_examples.h"
#include "lvgl/demos/lv_demos.h"
#include "glob.h"

#include "net_link.h"
#include "ui_shared.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static lv_display_t * hal_init(int32_t w, int32_t h);

/**********************
 *  STATIC VARIABLES
 **********************/

/********************** 
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

extern void freertos_main(void);

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *      VARIABLES
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/* ---------------- JCLINK 回调 ---------------- */
static void on_result(const char *json, void *arg)
{
  (void)arg;
  ui_shared_set_result(json);
}

static void on_image(const nl_image_t *img, void *arg)
{
  (void)arg;
  ui_shared_set_image(img->data, img->len, img->fmt, img->width, img->height);
}

static void on_log(const char *line, void *arg)
{
  (void)arg;
  ui_shared_add_log(line);
}

static void on_state(bool connected, const char *detail, void *arg)
{
  (void)arg;
  ui_shared_set_link(connected, detail);
}

static void on_action(const char *cmd_json, void *arg)
{
  (void)arg;
  net_link_send_cmd(cmd_json);
}

/* 调试用自动截图：SIM_SHOT=/path/prefix 时每 SIM_SHOT_MS 毫秒存一张 BMP */
static void shot_timer_cb(lv_timer_t *t)
{
  (void)t;
  const char *prefix = getenv("SIM_SHOT");
  if (!prefix) return;
  lv_display_t *d = lv_display_get_default();
  SDL_Renderer *r = lv_sdl_window_get_renderer(d);
  if (!r) return;
  int w = lv_display_get_horizontal_resolution(d);
  int h = lv_display_get_vertical_resolution(d);
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!s) return;
  SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
  static int n = 0;
  char path[512];
  snprintf(path, sizeof(path), "%s_%03d.bmp", prefix, ++n);
  if (SDL_SaveBMP(s, path) == 0) {
    printf("[shot] %s\n", path);
  }
  SDL_FreeSurface(s);
}

int main(int argc, char **argv)
{
  const char *host = getenv("SIM_HOST") ? getenv("SIM_HOST") : "192.168.33.2";
  int port = getenv("SIM_PORT") ? atoi(getenv("SIM_PORT")) : 3333;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--host") && i + 1 < argc) {
      host = argv[++i];
    } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
      port = atoi(argv[++i]);
    }
  }

  /*Initialize LVGL*/
  lv_init();

  /*Initialize the HAL (display, input devices, tick) for LVGL*/
  hal_init(480, 800);

#if LV_USE_OS == LV_OS_NONE
  ui_shared_init();
  ui_shared_set_action_cb(on_action, NULL);

  net_link_set_callbacks(on_result, on_image, on_log, on_state, NULL);
  net_link_start(host, port);

  const char *shot = getenv("SIM_SHOT");
  if (shot) {
    int ms = getenv("SIM_SHOT_MS") ? atoi(getenv("SIM_SHOT_MS")) : 3000;
    lv_timer_create(shot_timer_cb, ms, NULL);
    printf("[shot] auto screenshot every %d ms -> %s_%%03d.bmp\n", ms, shot);
  }

  printf("JCLINK simulator -> %s:%d (window 480x800)\n", host, port);

  while(1) {
    /* Periodically call the lv_task handler.
     * It could be done in a timer interrupt or an OS task too.*/
    lv_timer_handler();
    net_link_tick();
    ui_shared_tick();
    usleep(5 * 1000);
  }

#elif LV_USE_OS == LV_OS_FREERTOS

  /* Run FreeRTOS and create lvgl task */
  freertos_main();  

#endif

  return 0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Initialize the Hardware Abstraction Layer (HAL) for the LVGL graphics
 * library
 */
static lv_display_t * hal_init(int32_t w, int32_t h)
{

  lv_group_set_default(lv_group_create());

  lv_display_t * disp = lv_sdl_window_create(w, h);

  lv_indev_t * mouse = lv_sdl_mouse_create();
  lv_indev_set_group(mouse, lv_group_get_default());
  lv_indev_set_display(mouse, disp);
  lv_display_set_default(disp);

  LV_IMAGE_DECLARE(mouse_cursor_icon); /*Declare the image file.*/
  lv_obj_t * cursor_obj;
  cursor_obj = lv_image_create(lv_screen_active()); /*Create an image object for the cursor */
  lv_image_set_src(cursor_obj, &mouse_cursor_icon);           /*Set the image source*/
  lv_indev_set_cursor(mouse, cursor_obj);             /*Connect the image  object to the driver*/

  lv_indev_t * mousewheel = lv_sdl_mousewheel_create();
  lv_indev_set_display(mousewheel, disp);
  lv_indev_set_group(mousewheel, lv_group_get_default());

  lv_indev_t * kb = lv_sdl_keyboard_create();
  lv_indev_set_display(kb, disp);
  lv_indev_set_group(kb, lv_group_get_default());

  return disp;
}