#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#include <pthread.h>
#include <math.h>

// sudo apt install libfftw3-dev
#include <fftw3.h>

#include "timing.h"
#include "if_subsample.h"
#include "graphics.h"

/* Input from if_subsample.c */
extern if_fft_buffer_t if_fft_buffer;

#define FFT_SIZE    256 //2048
//#define FFT_TIME_SMOOTH 0.999f // 0.0 - 1.0
#define FFT_TIME_SMOOTH 0.4f // 0.0 - 1.0

static float hanning_window_const[FFT_SIZE];
static float hamming_window_const[FFT_SIZE];

static fftwf_complex* fft_in;
static fftwf_complex* fft_out;
static fftwf_plan fft_plan;

static float fft_data_staging[FFT_SIZE];
static float fft_scaled_data[FFT_SIZE];
static uint8_t fft_data_output[FFT_SIZE];

void if_fft_init(void)
{
    for(int i=0; i<FFT_SIZE; i++)
    {
        /* Hanning */
        hanning_window_const[i] = 0.5 * (1.0 - cos(2*M_PI*(((float)i)/FFT_SIZE)));

        /* Hamming */
        hamming_window_const[i] = 0.54 - (0.46 * cos(2*M_PI*(0.5+((2.0*((float)i/(FFT_SIZE-1))+1.0)/2))));
    }

    /* Set up FFTW */
    fft_in = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * FFT_SIZE);
    fft_out = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * FFT_SIZE);
    fft_plan = fftwf_plan_dft_1d(FFT_SIZE, fft_in, fft_out, FFTW_FORWARD, FFTW_PATIENT);
    printf(" "); fftwf_print_plan(fft_plan); printf("\n");
}

static void fft_fftw_close(void)
{
    /* De-init fftw */
    fftwf_free(fft_in);
    fftwf_free(fft_out);
    fftwf_destroy_plan(fft_plan);
    fftwf_forget_wisdom();
}

/* IF_FFT Thread */
void *if_fft_thread(void *arg)
{
    bool *exit_requested = (bool *)arg;

    int i, offset;
    fftw_complex pt;
    double pwr, lpwr;

    double pwr_scale = 1.0 / ((float)FFT_SIZE * (float)FFT_SIZE);

    struct timespec ts;

    uint64_t last_output = monotonic_ms();

    /* Set pthread timer on .signal to use monotonic clock */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init (&if_fft_buffer.signal, &attr);
    pthread_condattr_destroy(&attr);

    while(false == *exit_requested)
    {
        /* Lock input buffer */
        pthread_mutex_lock(&if_fft_buffer.mutex);

        while(if_fft_buffer.index >= (if_fft_buffer.size/(FFT_SIZE * sizeof(float) * 2))
            && false == *exit_requested)
        {
            /* Set timer for 100ms */
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += 10 * 1000000;

            pthread_cond_timedwait(&if_fft_buffer.signal, &if_fft_buffer.mutex, &ts);
        }

        if(*exit_requested)
        {
            break;
        }

        offset = if_fft_buffer.index * FFT_SIZE * 2;

        /* Copy data out of rf buffer into fft_input buffer */
        for (i = 0; i < FFT_SIZE; i++)
        {
            fft_in[i][0] = (((float*)if_fft_buffer.data)[offset+(2*i)]) * hamming_window_const[i];
            fft_in[i][1] = (((float*)if_fft_buffer.data)[offset+(2*i)+1]) * hamming_window_const[i];
            //printf("D in: %.4f, %.4f\n",
            //    ((float*)if_fft_buffer.data)[offset+(2*i)],
            //    ((float*)if_fft_buffer.data)[offset+(2*i)+1]);
        }

        if_fft_buffer.index++;

        /* Unlock input buffer */
        pthread_mutex_unlock(&if_fft_buffer.mutex);

        /* Run FFT */
        fftwf_execute(fft_plan);

        float int_max = -9999.0;
        float int_min = 9999.0;

        for (i = 0; i < FFT_SIZE; i++)
        {
            /* shift, normalize and convert to dBFS */
            if (i < FFT_SIZE / 2)
            {
                pt[0] = fft_out[FFT_SIZE / 2 + i][0] / FFT_SIZE;
                pt[1] = fft_out[FFT_SIZE / 2 + i][1] / FFT_SIZE;
            }
            else
            {
                pt[0] = fft_out[i - FFT_SIZE / 2][0] / FFT_SIZE;
                pt[1] = fft_out[i - FFT_SIZE / 2][1] / FFT_SIZE;
            }
            pwr = pwr_scale * (pt[0] * pt[0]) + (pt[1] * pt[1]);
            lpwr = 10.f * log10(pwr + 1.0e-20);
            
            fft_data_staging[i] = (lpwr * (1.f - FFT_TIME_SMOOTH)) + (fft_data_staging[i] * FFT_TIME_SMOOTH);

            //printf("%f\n", fft_data_staging[i]);

            fft_scaled_data[i] = 15 * (fft_data_staging[i] + 79);

            if(fft_scaled_data[i] > int_max) int_max = fft_scaled_data[i];
            if(fft_scaled_data[i] < int_min) int_min = fft_scaled_data[i];

            if(fft_scaled_data[i] < 0) fft_scaled_data[i] = 0;
            if(fft_scaled_data[i] > 255) fft_scaled_data[i] = 255;

            fft_data_output[i] = (uint8_t)(fft_scaled_data[i]);
        }
        //printf("Max: %f, Min %f\n", int_max, int_min);

        //ws_fft_submit((uint8_t *)fft_data_staging, (FFT_SIZE * sizeof(float)));

        if(monotonic_ms() > (last_output + 30))
        {
            graphics_if_fft_newdata(fft_data_output);
            last_output = monotonic_ms();
  
            /* Trigger Frequency Render (a bit hacky) */
            graphics_frequency_newdata();
        }
    }

    fft_fftw_close();

    return NULL;
}