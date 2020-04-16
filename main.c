#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <fftw3.h>

#include "screen.h"
#include "mouse.h"
#include "timing.h"
#include "graphics.h"

#include "lime.h"
#include "fft.h"
#include "buffer/buffer_circular.h"
#include "if_subsample.h"
#include "if_fft.h"
#include "if_demod.h"
#include "audio.h"
#include "touch.h"

static bool app_exit = false;

void sigint_handler(int sig)
{
    (void)sig;
    app_exit = true;
}

static pthread_t screen_thread_obj;
static pthread_t touch_thread_obj;
static pthread_t mouse_thread_obj;
static pthread_t if_subsample_thread_obj;
static pthread_t if_fft_thread_obj;
static pthread_t if_demod_thread_obj;
static pthread_t audio_rx_thread_obj;
static pthread_t lime_thread_obj;
static pthread_t fft_thread_obj;

int main(int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);

  /* Initialise screen and splash */
  if(!screen_init())
  {
    fprintf(stderr, "Error initialising screen!\n");
    return 1;
  }

  printf("Profiling FFTs..\n");
  fftwf_import_wisdom_from_filename(".fftwf_wisdom");
  printf(" - Main Band FFT\n");
  main_fft_init();
  printf(" - IF Band FFT\n");
  if_fft_init();
  printf(" - IF Demodulator FFTs\n");
  if_demod_init();
  fftwf_export_wisdom_to_filename(".fftwf_wisdom");
  printf("FFTs Done.\n");

  /* Touchscreen Thread */
  if(pthread_create(&touch_thread_obj, NULL, touch_thread, &app_exit))
  {
      fprintf(stderr, "Error creating %s pthread\n", "Touch");
      return 1;
  }
  pthread_setname_np(touch_thread_obj, "Touch");

  /* Mouse Listener Thread */
  if(pthread_create(&mouse_thread_obj, NULL, mouse_thread, &app_exit))
  {
      fprintf(stderr, "Error creating %s pthread\n", "Mouse");
      return 1;
  }
  pthread_setname_np(mouse_thread_obj, "Mouse");

  /* Setting up buffers */
  buffer_circular_init(&buffer_circular_iq_main, sizeof(buffer_iqsample_t), 4096*1024);
  buffer_circular_init(&buffer_circular_iq_if, sizeof(buffer_iqsample_t), 64*1024);
  buffer_circular_init(&buffer_circular_audio, sizeof(int16_t), 2*1024);

  /* IF Subsample Thread */
  if(pthread_create(&if_subsample_thread_obj, NULL, if_subsample_thread, &app_exit))
  {
      fprintf(stderr, "Error creating %s pthread\n", "IF Subsample");
      return 1;
  }
  pthread_setname_np(if_subsample_thread_obj, "IF Subsample");

  /* IF FFT Thread */
  if(pthread_create(&if_fft_thread_obj, NULL, if_fft_thread, &app_exit))
  {
      fprintf(stderr, "Error creating %s pthread\n", "IF FFT");
      return 1;
  }
  pthread_setname_np(if_fft_thread_obj, "IF FFT");

  /* IF Demodulator Thread */
  if(pthread_create(&if_demod_thread_obj, NULL, if_demod_thread, &app_exit))
  {
      fprintf(stderr, "Error creating %s pthread\n", "IF Demod");
      return 1;
  }
  pthread_setname_np(if_demod_thread_obj, "IF Demod");

  /* Audio RX Thread */
  if(pthread_create(&audio_rx_thread_obj, NULL, audio_playback_thread, &app_exit))
  {
      fprintf(stderr, "Error creating %s pthread\n", "RX Audio");
      return 1;
  }
  pthread_setname_np(audio_rx_thread_obj, "RX Audio");

  /* LimeSDR Thread */
  if(pthread_create(&lime_thread_obj, NULL, lime_thread, &app_exit))
  {
      fprintf(stderr, "Error creating %s pthread\n", "Lime");
      return 1;
  }
  pthread_setname_np(lime_thread_obj, "Lime");

  /* Band FFT Thread */
  if(pthread_create(&fft_thread_obj, NULL, fft_thread, &app_exit))
  {
      fprintf(stderr, "Error creating %s pthread\n", "FFT");
      return 1;
  }
  pthread_setname_np(fft_thread_obj, "FFT");

  /* Screen Render (backbuffer -> screen) Thread */
  if(pthread_create(&screen_thread_obj, NULL, screen_thread, &app_exit))
  {
      fprintf(stderr, "Error creating %s pthread\n", "Screen");
      return 1;
  }
  pthread_setname_np(screen_thread_obj, "Screen");

  while(!app_exit)
  {
    sleep_ms(10);
  }

  printf("Got SIGTERM/INT..\n");
  app_exit = true;

  /* TODO: Add validity flag and appropriate bomb-out logic for buffer-consumer threads so we don't segfault on exit half the time */

  printf("Waiting for Lime Thread to exit..\n");
  pthread_join(lime_thread_obj, NULL);
  printf("Waiting for FFT Thread to exit..\n");
  pthread_join(fft_thread_obj, NULL);
  //pthread_join(mouse_thread_obj, NULL);
  printf("Waiting for Screen Thread to exit..\n");
  pthread_join(screen_thread_obj, NULL);

  printf("All threads caught, exiting..\n");
}
