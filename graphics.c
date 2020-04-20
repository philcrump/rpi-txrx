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

#define IF_SPECTRUM_POS_X     (SCREEN_WIDTH - IF_SPECTRUM_WIDTH - 1)
#define IF_SPECTRUM_POS_Y     (FREQUENCY_POS_Y + FREQUENCY_HEIGHT + 8)

/** IF Waterfall Display **/

#define IF_WATERFALL_WIDTH      256
#define IF_WATERFALL_HEIGHT     120
screen_pixel_t if_waterfall_buffer[IF_WATERFALL_HEIGHT][IF_WATERFALL_WIDTH] __attribute__ ((aligned (NEON_ALIGNMENT)));

#define IF_WATERFALL_POS_X    (SCREEN_WIDTH - IF_WATERFALL_WIDTH)
#define IF_WATERFALL_POS_Y    (IF_SPECTRUM_POS_Y + IF_SPECTRUM_HEIGHT)

/** PTT Button **/

#define PTT_BUTTON_WIDTH      150
#define PTT_BUTTON_HEIGHT     100
screen_pixel_t ptt_button_buffer[PTT_BUTTON_HEIGHT][PTT_BUTTON_WIDTH] __attribute__ ((aligned (NEON_ALIGNMENT)));

#define PTT_BUTTON_POS_X    (SCREEN_WIDTH - 5 - PTT_BUTTON_WIDTH)
#define PTT_BUTTON_POS_Y    (SCREEN_HEIGHT - 5 - PTT_BUTTON_HEIGHT)


const screen_pixel_t graphics_white_pixel =
{
  .Alpha = 0x80,
  .Red = 0xFF,
  .Green = 0xFF,
  .Blue = 0xFF
};

const screen_pixel_t graphics_black_pixel =
{
  .Alpha = 0x80,
  .Red = 0x00,
  .Green = 0x00,
  .Blue = 0x00
};

const screen_pixel_t graphics_red_pixel =
{
  .Alpha = 0x80,
  .Red = 0xFF,
  .Green = 0x00,
  .Blue = 0x00
};

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

void ptt_button_render_font_cb(int x, int y, screen_pixel_t *pixel_ptr)
{
  memcpy(&(ptt_button_buffer[y][x]), pixel_ptr, sizeof(screen_pixel_t));
}

extern bool ptt_pressed;
void ptt_button_generate(void)
{
  uint32_t i, j;

  for(i = 0; i < PTT_BUTTON_HEIGHT; i++)
  {
    memcpy(&(ptt_button_buffer[i][0]), &graphics_white_pixel, sizeof(screen_pixel_t));
    memcpy(&(ptt_button_buffer[i][PTT_BUTTON_WIDTH-1]), &graphics_white_pixel, sizeof(screen_pixel_t));
  }

  for(j = 0; j < PTT_BUTTON_WIDTH; j++)
  {
    memcpy(&(ptt_button_buffer[0][j]), &graphics_white_pixel, sizeof(screen_pixel_t));
    memcpy(&(ptt_button_buffer[PTT_BUTTON_HEIGHT-1][j]), &graphics_white_pixel, sizeof(screen_pixel_t));
  }

  const screen_pixel_t *ptt_button_background_pixel_ptr;
  ptt_button_background_pixel_ptr = &graphics_black_pixel;
  if(ptt_pressed)
  {
    ptt_button_background_pixel_ptr = &graphics_red_pixel;
  }

  for(i = 0+1; i < PTT_BUTTON_HEIGHT-1; i++)
  {
    for(j = 0+1; j < PTT_BUTTON_WIDTH-1; j++)
    {
      memcpy(&(ptt_button_buffer[i][j]), ptt_button_background_pixel_ptr, sizeof(screen_pixel_t));
    }
  }

  char ptt_string[] = "PTT";
  font_render_colour_string_with_callback(
    (PTT_BUTTON_WIDTH - font_width_string(&font_dejavu_sans_36, ptt_string)) / 2,
    (PTT_BUTTON_HEIGHT - font_dejavu_sans_36.height) / 2,
    &font_dejavu_sans_36, ptt_button_background_pixel_ptr, &graphics_white_pixel,
    ptt_string, ptt_button_render_font_cb
  );
}

void ptt_button_render(void)
{
  for(uint32_t i = 0; i < PTT_BUTTON_HEIGHT; i++)
  {
    screen_setPixelLine(PTT_BUTTON_POS_X, PTT_BUTTON_POS_Y + i, PTT_BUTTON_WIDTH, ptt_button_buffer[i]);
  }
}

static void spectrum_generate(uint8_t *fft_data)
{
  const screen_pixel_t selected_marker_pixel =
  {
    .Alpha = 0x80,
    .Red = 0x50,
    .Green = 0x50,
    .Blue = 0x50
  };

  const screen_pixel_t selected_band_pixel =
  {
    .Alpha = 0x80,
    .Red = 0x1A,
    .Green = 0x1A,
    .Blue = 0x1A
  };

  uint32_t value;
  uint32_t i, j;

  for(i = 0; i < MAIN_SPECTRUM_HEIGHT; i++)
  {
    for(j = 0; j < MAIN_SPECTRUM_WIDTH; j++)
    {
      memcpy(&(main_spectrum_buffer[i][j]), &graphics_black_pixel, sizeof(screen_pixel_t));
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
      memcpy(&(main_spectrum_buffer[j][i]), &graphics_white_pixel, sizeof(screen_pixel_t));
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

static void frequency_render_font_cb(int x, int y, screen_pixel_t *pixel_ptr)
{
  memcpy(&(frequency_buffer[y][x]), pixel_ptr, sizeof(screen_pixel_t));
}

static void frequency_generate(void)
{
  uint32_t i, j;
  /* Clear buffer */
  for(i = 0; i < FREQUENCY_HEIGHT; i++)
  {
    for(j = 0; j <FREQUENCY_WIDTH; j++)
    {
      memcpy(&(frequency_buffer[i][j]), &graphics_black_pixel, sizeof(screen_pixel_t));
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

static void frequency_render(void)
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

  ptt_button_generate();
  ptt_button_render();
}

static void if_waterfall_generate(uint32_t counter, uint8_t *fft_data)
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

static void if_waterfall_render(uint32_t counter)
{
  for(uint32_t i = 0; i < IF_WATERFALL_HEIGHT; i++)
  {
    screen_setPixelLine(
      IF_WATERFALL_POS_X, IF_WATERFALL_POS_Y + i,
      IF_WATERFALL_WIDTH, if_waterfall_buffer[(i + counter) % IF_WATERFALL_HEIGHT]);
  }
}

static void if_spectrum_generate(uint8_t *fft_data)
{
  const screen_pixel_t selected_marker_pixel =
  {
    .Alpha = 0x80,
    .Red = 0x50,
    .Green = 0x50,
    .Blue = 0x50
  };

  const screen_pixel_t selected_band_pixel =
  {
    .Alpha = 0x80,
    .Red = 0x1A,
    .Green = 0x1A,
    .Blue = 0x1A
  };

  uint32_t value;
  uint32_t i, j;

  for(i = 0; i < IF_SPECTRUM_HEIGHT; i++)
  {
    for(j = 0; j < IF_SPECTRUM_WIDTH; j++)
    {
      memcpy(&(if_spectrum_buffer[i][j]), &graphics_black_pixel, sizeof(screen_pixel_t));
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
      memcpy(&(if_spectrum_buffer[j][i]), &graphics_white_pixel, sizeof(screen_pixel_t));
    }
  }
}

static void if_spectrum_render(void) 
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