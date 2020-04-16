#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../screen.h"
#include "font.h"

/* Returns Width of rendered character, 0 if not found */
uint32_t font_render_character_with_callback(int x, int y, const font_t *font_ptr, char c, void (*render_cb)(int x, int y, screen_pixel_t *pixel_ptr))
{
    screen_pixel_t character_pixel;
    character_pixel.Alpha = 0x80;

    uint32_t y_offset = 0;
    if(font_ptr->characters[(uint8_t)c].height < font_ptr->ascent)
    {
        y_offset = font_ptr->ascent - font_ptr->characters[(uint8_t)c].height;
    }

    /* For each Line */
    for(uint32_t i = 0; i < font_ptr->characters[(uint8_t)c].height; i++)
    {
        /* For each Column */
        for(uint32_t j = 0; j < font_ptr->characters[(uint8_t)c].width; j++)
        {
            character_pixel.Red = font_ptr->characters[(uint8_t)c].map[j+(i*font_ptr->characters[(uint8_t)c].width)];
            character_pixel.Green = font_ptr->characters[(uint8_t)c].map[j+(i*font_ptr->characters[(uint8_t)c].width)];
            character_pixel.Blue = font_ptr->characters[(uint8_t)c].map[j+(i*font_ptr->characters[(uint8_t)c].width)];

            render_cb(
                j + x,
                i + y + y_offset,
                &character_pixel
            );
        }
    }

    return font_ptr->characters[(uint8_t)c].render_width;
}

uint32_t font_render_string_with_callback(int x, int y, const font_t *font_ptr, char *string, void (*render_cb)(int x, int y, screen_pixel_t *pixel_ptr))
{
    uint32_t string_length = strlen(string);
    for(uint32_t i = 0; i < string_length; i++)
    {
        x += font_render_character_with_callback(x, y, font_ptr, string[i], render_cb);
    }
    return x;
}