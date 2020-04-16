#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <math.h>

#include "screen.h"
#include "graphics.h"
#include "font/font.h"

#define NEON_ALIGNMENT (4*4*2) // From libcsdr

int64_t lo_frequency = 9750000;
int64_t center_frequency = 10489750000;
int64_t span_frequency = 512000;

int64_t selected_span_frequency = 10240;
int64_t selected_center_frequency = 10489499950;

/** Main Waterfall Display **/

#define MAIN_WATERFALL_WIDTH    512
#define MAIN_WATERFALL_HEIGHT   300
screen_pixel_t main_waterfall_buffer[MAIN_WATERFALL_HEIGHT][MAIN_WATERFALL_WIDTH] __attribute__ ((aligned (NEON_ALIGNMENT)));

#define MAIN_WATERFALL_POS_X    0
#define MAIN_WATERFALL_POS_Y    (SCREEN_HEIGHT - MAIN_WATERFALL_HEIGHT)

/** Main Spectrum Display **/

#define MAIN_SPECTRUM_WIDTH     512
#define MAIN_SPECTRUM_HEIGHT    170
screen_pixel_t main_spectrum_buffer[MAIN_SPECTRUM_HEIGHT][MAIN_SPECTRUM_WIDTH] __attribute__ ((aligned (NEON_ALIGNMENT)));
#define MAIN_SPECTRUM_TIME_SMOOTH   0.8f
float main_spectrum_smooth_buffer[MAIN_SPECTRUM_WIDTH] = { 0 };

#define MAIN_SPECTRUM_POS_X     0
#define MAIN_SPECTRUM_POS_Y     (SCREEN_HEIGHT - MAIN_WATERFALL_HEIGHT - MAIN_SPECTRUM_HEIGHT)

/** Frequency Display **/

#define FREQUENCY_WIDTH         256
#define FREQUENCY_HEIGHT        43
screen_pixel_t frequency_buffer[FREQUENCY_HEIGHT][FREQUENCY_WIDTH] __attribute__ ((aligned (NEON_ALIGNMENT)));

#define FREQUENCY_POS_X         544
#define FREQUENCY_POS_Y         8

/** IF Spectrum Display **/

#define IF_SPECTRUM_WIDTH       256
#define IF_SPECTRUM_HEIGHT      100
screen_pixel_t if_spectrum_buffer[IF_SPECTRUM_HEIGHT][IF_SPECTRUM_WIDTH] __attribute__ ((aligned (NEON_ALIGNMENT)));
#define IF_SPECTRUM_TIME_SMOOTH 0.8f
float if_spectrum_smooth_buffer[IF_SPECTRUM_WIDTH] = { 0 };

#define IF_SPECTRUM_POS_X     (800 - IF_SPECTRUM_WIDTH - 1)
#define IF_SPECTRUM_POS_Y     (FREQUENCY_POS_Y + FREQUENCY_HEIGHT + 8)

/** IF Waterfall Display **/

#define IF_WATERFALL_WIDTH      256
#define IF_WATERFALL_HEIGHT     120
screen_pixel_t if_waterfall_buffer[IF_WATERFALL_HEIGHT][IF_WATERFALL_WIDTH] __attribute__ ((aligned (NEON_ALIGNMENT)));

#define IF_WATERFALL_POS_X    (800 - IF_WATERFALL_WIDTH)
#define IF_WATERFALL_POS_Y    (IF_SPECTRUM_POS_Y + IF_SPECTRUM_HEIGHT)

static void waterfall_cm_websdr(screen_pixel_t *pixel_ptr, uint8_t value)
{
  /* Raspberry Pi Display starts flickering the backlight below a certain intensity, ensure that we don't go below this (~70) */
  if(value < 64)
  {
    pixel_ptr->Red = 0;
    pixel_ptr->Green = 0;
    pixel_ptr->Blue = 70 + (1.5 * value);
  }
  else if(value < 128)
  {
    pixel_ptr->Red = (3 * value) - 192;
    pixel_ptr->Green = 0;
    pixel_ptr->Blue = 70 + (1.5 * value);
  }
  else if(value < 192)
  {
    pixel_ptr->Red = value + 64;
    pixel_ptr->Green = 256 * sqrt((value - 128) / 64);
    pixel_ptr->Blue = 512 - (2 * value);
  }
  else
  {
    pixel_ptr->Red = 255;
    pixel_ptr->Green = 255;
    pixel_ptr->Blue = 512 - (2 * value);
  }
}

static void waterfall_generate(uint32_t counter, uint8_t *fft_data)
{
  screen_pixel_t new_pixel;
  new_pixel.Alpha = 0x80;

  for(uint32_t i = 0; i < MAIN_WATERFALL_WIDTH; i++)
  {
    /* Greyscale */
    //new_pixel.Red = fft_data[i];
    //new_pixel.Green = fft_data[i];
    //new_pixel.Blue = fft_data[i];

    /* websdr colour map */
    waterfall_cm_websdr(&new_pixel, fft_data[i]);

    memcpy(&(main_waterfall_buffer[counter][i]), &new_pixel, sizeof(screen_pixel_t));
  }
}

static void waterfall_render(uint32_t counter)
{
  for(uint32_t i = 0; i < MAIN_WATERFALL_HEIGHT; i++)
  {
    screen_setPixelLine(MAIN_WATERFALL_POS_X, MAIN_WATERFALL_POS_Y + i, MAIN_WATERFALL_WIDTH, main_waterfall_buffer[(i + counter) % MAIN_WATERFALL_HEIGHT]);
  }
}

static void spectrum_generate(uint8_t *fft_data)
{
  screen_pixel_t blank_pixel;
  blank_pixel.Alpha = 0x80;
  blank_pixel.Red = 0x00;
  blank_pixel.Green = 0x00;
  blank_pixel.Blue = 0x00;

  screen_pixel_t selected_marker_pixel;
  selected_marker_pixel.Alpha = 0x80;
  selected_marker_pixel.Red = 0x50;
  selected_marker_pixel.Green = 0x50;
  selected_marker_pixel.Blue = 0x50;

  screen_pixel_t selected_band_pixel;
  selected_band_pixel.Alpha = 0x80;
  selected_band_pixel.Red = 0x1A;
  selected_band_pixel.Green = 0x1A;
  selected_band_pixel.Blue = 0x1A;

  screen_pixel_t spectrum_pixel;
  spectrum_pixel.Alpha = 0x80;
  spectrum_pixel.Red = 0xFF;
  spectrum_pixel.Green = 0xFF;
  spectrum_pixel.Blue = 0xFF;

  uint32_t value;
  uint32_t i, j;

  for(i = 0; i < MAIN_SPECTRUM_HEIGHT; i++)
  {
    for(j = 0; j < MAIN_SPECTRUM_WIDTH; j++)
    {
      memcpy(&(main_spectrum_buffer[i][j]), &blank_pixel, sizeof(screen_pixel_t));
    }
  }

  /* Draw selected band markers */
  int32_t start_marker = 
      (((selected_center_frequency - (selected_span_frequency / 2))
       - (center_frequency - (span_frequency/2))) * MAIN_SPECTRUM_WIDTH) / span_frequency;

  int32_t end_marker = 
      (((selected_center_frequency + (selected_span_frequency / 2))
       - (center_frequency - (span_frequency/2))) * MAIN_SPECTRUM_WIDTH) / span_frequency;

  uint32_t start_selected = start_marker > 0 ? start_marker : 0;
  uint32_t end_selected = end_marker < (int32_t)(MAIN_SPECTRUM_WIDTH - 1) ? (uint32_t)end_marker : (MAIN_SPECTRUM_WIDTH - 1);

  for(i = 0; i < MAIN_SPECTRUM_HEIGHT; i++)
  {
    /* Start Marker, if in view */
    if(start_marker >= 0)
    {
      memcpy(&(main_spectrum_buffer[i][start_marker]), &selected_marker_pixel, sizeof(screen_pixel_t));
    }
    /* Highlighted section */
    for(j = start_selected+1; j<end_selected; j++)
    {
      memcpy(&(main_spectrum_buffer[i][j]), &selected_band_pixel, sizeof(screen_pixel_t));
    }
    /* End Marker, if in view */
    if(end_marker < (int32_t)MAIN_SPECTRUM_WIDTH)
    {
      memcpy(&(main_spectrum_buffer[i][end_marker]), &selected_marker_pixel, sizeof(screen_pixel_t));
    }
  }

  /* Draw FFT Spectrum */
  for(i = 0; i < MAIN_SPECTRUM_WIDTH; i++)
  {
    main_spectrum_smooth_buffer[i] = (((float)fft_data[i]) * (1.f - MAIN_SPECTRUM_TIME_SMOOTH)) + (main_spectrum_smooth_buffer[i] * MAIN_SPECTRUM_TIME_SMOOTH);
    value = ((uint32_t)(main_spectrum_smooth_buffer[i] * MAIN_SPECTRUM_HEIGHT)) / 255;

    for(j = (MAIN_SPECTRUM_HEIGHT-1); j > MAIN_SPECTRUM_HEIGHT-value-1; j--)
    {
      memcpy(&(main_spectrum_buffer[j][i]), &spectrum_pixel, sizeof(screen_pixel_t));
    }
  }
}

static void spectrum_render(void)
{
  for(uint32_t i = 0; i < MAIN_SPECTRUM_HEIGHT; i++)
  {
    screen_setPixelLine(MAIN_SPECTRUM_POS_X, MAIN_SPECTRUM_POS_Y + i, MAIN_SPECTRUM_WIDTH, main_spectrum_buffer[i]);
  }
}

void frequency_render_font_cb(int x, int y, screen_pixel_t *pixel_ptr)
{
  memcpy(&(frequency_buffer[y][x]), pixel_ptr, sizeof(screen_pixel_t));
}

void frequency_generate(void)
{
  screen_pixel_t blank_pixel;
  blank_pixel.Alpha = 0x80;
  blank_pixel.Red = 0x00;
  blank_pixel.Green = 0x00;
  blank_pixel.Blue = 0x00;

  /* Clear buffer */
  for(uint32_t i = 0; i < FREQUENCY_HEIGHT; i++)
  {
    for(uint32_t j = 0; j <FREQUENCY_WIDTH; j++)
    {
      memcpy(&(frequency_buffer[i][j]), &blank_pixel, sizeof(screen_pixel_t));
    }
  }

  char *freq_string;
  asprintf(&freq_string, ".%3lld.%03lld.%03lld", 
    (selected_center_frequency / 1000000) % 1000,
    (selected_center_frequency / 1000) % 1000,
    selected_center_frequency % 1000);
  font_render_string_with_callback(0, 0, &font_dejavu_sans_36, freq_string, frequency_render_font_cb);
  free(freq_string);
}

void frequency_render(void)
{
  /* Render Frequency display buffer */
  for(uint32_t i = 0; i < FREQUENCY_HEIGHT; i++)
  {
    screen_setPixelLine(FREQUENCY_POS_X, FREQUENCY_POS_Y + i, FREQUENCY_WIDTH, frequency_buffer[i]);
  }
}

static uint32_t main_waterfall_counter = (MAIN_WATERFALL_HEIGHT-1);
/* Takes 512byte FFT */
void waterfall_render_fft(uint8_t *fft_data)
{
#if 0
  for(uint32_t i = 0; i < MAIN_SPECTRUM_WIDTH; i++)
  {
    printf("%d,", fft_data[i]);
  }
  printf("\n");
#endif

  waterfall_generate(main_waterfall_counter, fft_data);
  spectrum_generate(fft_data);

  waterfall_render(main_waterfall_counter);
  spectrum_render();

  if(main_waterfall_counter-- == 0) main_waterfall_counter = (MAIN_WATERFALL_HEIGHT-1);
}

void graphics_frequency_newdata(void)
{
  frequency_generate();
  frequency_render();
}

 void if_waterfall_generate(uint32_t counter, uint8_t *fft_data)
{
  screen_pixel_t new_pixel;
  new_pixel.Alpha = 0x80;

  for(uint32_t i = 0; i < IF_WATERFALL_WIDTH; i++)
  {
    /* Greyscale */
    //new_pixel.Red = fft_data[i];
    //new_pixel.Green = fft_data[i];
    //new_pixel.Blue = fft_data[i];

    /* websdr colour map */
    waterfall_cm_websdr(&new_pixel, fft_data[i]);

    memcpy(&(if_waterfall_buffer[counter][i]), &new_pixel, sizeof(screen_pixel_t));
  }
}

 void if_waterfall_render(uint32_t counter)
{
  for(uint32_t i = 0; i < IF_WATERFALL_HEIGHT; i++)
  {
    screen_setPixelLine(
      IF_WATERFALL_POS_X, IF_WATERFALL_POS_Y + i,
      IF_WATERFALL_WIDTH, if_waterfall_buffer[(i + counter) % IF_WATERFALL_HEIGHT]);
  }
}

 void if_spectrum_generate(uint8_t *fft_data)
{
  screen_pixel_t blank_pixel;
  blank_pixel.Alpha = 0x80;
  blank_pixel.Red = 0x00;
  blank_pixel.Green = 0x00;
  blank_pixel.Blue = 0x00;

  screen_pixel_t selected_marker_pixel;
  selected_marker_pixel.Alpha = 0x80;
  selected_marker_pixel.Red = 0x50;
  selected_marker_pixel.Green = 0x50;
  selected_marker_pixel.Blue = 0x50;

  screen_pixel_t selected_band_pixel;
  selected_band_pixel.Alpha = 0x80;
  selected_band_pixel.Red = 0x1A;
  selected_band_pixel.Green = 0x1A;
  selected_band_pixel.Blue = 0x1A;

  screen_pixel_t spectrum_pixel;
  spectrum_pixel.Alpha = 0x80;
  spectrum_pixel.Red = 0xFF;
  spectrum_pixel.Green = 0xFF;
  spectrum_pixel.Blue = 0xFF;

  uint32_t value;
  uint32_t i, j;

  for(i = 0; i < IF_SPECTRUM_HEIGHT; i++)
  {
    for(j = 0; j < IF_SPECTRUM_WIDTH; j++)
    {
      memcpy(&(if_spectrum_buffer[i][j]), &blank_pixel, sizeof(screen_pixel_t));
    }
  }

  /* Draw SSB demod markers */
  uint32_t start_marker = (0.5 + ((float)200 / selected_span_frequency)) * IF_SPECTRUM_WIDTH;
  uint32_t end_marker = (0.5 + (((float)200 + 2700.0) / selected_span_frequency)) * IF_SPECTRUM_WIDTH;

  for(i = 0; i < IF_SPECTRUM_HEIGHT; i++)
  {
    /* Start Marker*/
    memcpy(&(if_spectrum_buffer[i][start_marker]), &selected_marker_pixel, sizeof(screen_pixel_t));
    /* Highlighted section */
    for(j = start_marker + 1; j < end_marker; j++)
    {
      memcpy(&(if_spectrum_buffer[i][j]), &selected_band_pixel, sizeof(screen_pixel_t));
    }
    /* End Marker */
    memcpy(&(if_spectrum_buffer[i][end_marker]), &selected_marker_pixel, sizeof(screen_pixel_t));
  }

  /* Draw FFT Spectrum */
  for(i = 0; i < IF_SPECTRUM_WIDTH; i++)
  {
    if_spectrum_smooth_buffer[i] = (((float)fft_data[i]) * (1.f - IF_SPECTRUM_TIME_SMOOTH)) + (if_spectrum_smooth_buffer[i] * IF_SPECTRUM_TIME_SMOOTH);
    value = ((uint32_t)(if_spectrum_smooth_buffer[i] * IF_SPECTRUM_HEIGHT)) / 255;

    for(j = (IF_SPECTRUM_HEIGHT - 1); j > IF_SPECTRUM_HEIGHT-value-1; j--)
    {
      memcpy(&(if_spectrum_buffer[j][i]), &spectrum_pixel, sizeof(screen_pixel_t));
    }
  }
}

 void if_spectrum_render(void) 
{
  for(uint32_t i = 0; i < IF_SPECTRUM_HEIGHT; i++)
  {
    screen_setPixelLine(IF_SPECTRUM_POS_X, IF_SPECTRUM_POS_Y + i, IF_SPECTRUM_WIDTH, if_spectrum_buffer[i]);
  }
}

static uint32_t if_waterfall_counter = IF_WATERFALL_HEIGHT;
void graphics_if_fft_newdata(uint8_t *fft_data)
{
#if 0
  for(uint32_t i = 0; i < IF_SPECTRUM_WIDTH; i++)
  {
    printf("%d,", fft_data[i]);
  }
  printf("\n");
#endif

  if_waterfall_generate(if_waterfall_counter, fft_data);
  if_spectrum_generate(fft_data);

  if_waterfall_render(if_waterfall_counter);
  if_spectrum_render();

  if(if_waterfall_counter-- == 0) if_waterfall_counter = (IF_WATERFALL_HEIGHT - 1);
}