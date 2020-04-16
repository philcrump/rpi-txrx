#ifndef __FONT_H__
#define __FONT_H__

#ifndef __SCREEN_H__
  typedef void screen_pixel_t;
#endif

typedef struct {
    const uint32_t height;
    const uint32_t width;
    const uint32_t render_width;
    const uint8_t *map;
} font_character_t;

typedef struct {
    const uint32_t height;
    const uint32_t ascent;
    const font_character_t *characters;
} font_t;

uint32_t font_render_string_with_callback(int x, int y, const font_t *font_ptr, char *string, void (*render_cb)(int x, int y, screen_pixel_t *pixel_ptr));

uint32_t font_render_character_with_callback(int x, int y, const font_t *font_ptr, char c, void (*render_cb)(int x, int y, screen_pixel_t *pixel_ptr));

#include "dejavu_sans_32.h"
#include "dejavu_sans_36.h"
#include "dejavu_sans_72.h"

#endif /* __FONT_H__ */