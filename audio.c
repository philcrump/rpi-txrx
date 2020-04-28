#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <alsa/asoundlib.h>

#include "buffer/buffer_circular.h"
#include "timing.h"

#define PCM_DEVICE "default"
#define SAMPLERATE  10240 //10549 //5800 //5439
#define CHANNELS    1

/* Samplerate should be 512000 / 50 = 10240 sps
 in csdr example: 2.4Ms/s / 50 = 48000 sps
*/

/* Audio Playback Thread */
void *audio_playback_thread(void *arg)
{
    bool *exit_requested = (bool *)arg;

    int pcm;
    snd_pcm_t *pcm_handle;
    snd_pcm_hw_params_t *params;
    snd_pcm_uframes_t buffer_min, buffer_max;
    char *buff;
    int buff_size;

    /* Open PCM device in playback */
    if ((pcm = snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0)) < 0) 
    {
        fprintf(stderr, "Audio: Fatal Error: Can't open \"%s\" PCM device. (%s)\n", PCM_DEVICE, snd_strerror(pcm));
        return NULL;
    }

    /* Allocate parameters object and fill it with default values*/
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_handle, params);

    /* Set parameters */
    if ((pcm = snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't set interleaved mode. %s\n", snd_strerror(pcm));
        return NULL;
    }

    if ((pcm = snd_pcm_hw_params_set_format(pcm_handle, params, SND_PCM_FORMAT_S16_LE)) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't set format. %s\n", snd_strerror(pcm));
        return NULL;
    }

    if ((pcm = snd_pcm_hw_params_set_channels(pcm_handle, params, CHANNELS)) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't set channels number. %s\n", snd_strerror(pcm));
        return NULL;
    }

    unsigned int exact_rate = SAMPLERATE;
    if (snd_pcm_hw_params_set_rate_near(pcm_handle, params, &exact_rate, 0) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't set samplerate %d, %s\n", SAMPLERATE, snd_strerror(pcm));
        return NULL;
    }
    if (exact_rate != SAMPLERATE)
    {
        printf("Audio: Warning: samplerate %d Hz is not supported by the hardware. Using %d Hz instead.\n", SAMPLERATE, exact_rate);
    }

    snd_pcm_uframes_t period_min;
    if ((pcm = snd_pcm_hw_params_get_period_size_min(params, &period_min, NULL)) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't get minimum period, %s\n", snd_strerror(pcm));
        return NULL;
    }

    snd_pcm_uframes_t period_max;
    if ((pcm = snd_pcm_hw_params_get_period_size_max(params, &period_max, NULL)) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't get maximum period, %s\n", snd_strerror(pcm));
        return NULL;
    }
    printf("Audio: allowable period values: %ld - %ld\n", period_min, period_max);

    int periodsize = period_min;
    /*
    if ((pcm = snd_pcm_hw_params_set_periods(pcm_handle, params, periodsize, 0)) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't set periods, %s\n", snd_strerror(pcm));
        return NULL;
    }
    */

    if ((pcm = snd_pcm_hw_params_get_buffer_size_min(params, &buffer_min)) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't get minimum buffer, %s\n", snd_strerror(pcm));
        return NULL;
    }

    if ((pcm = snd_pcm_hw_params_get_buffer_size_max(params, &buffer_max)) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't get maximum buffer, %s\n", snd_strerror(pcm));
        return NULL;
    }
    printf("Audio: allowable buffer values: %ld - %ld\n", buffer_min, buffer_max);

    snd_pcm_uframes_t buffer_size = SAMPLERATE / 2;
    if ((pcm = snd_pcm_hw_params_set_buffer_size_near(pcm_handle, params, &buffer_size)) < 0) 
    {
        fprintf(stderr, "Audio: Fatal Error: Can't set buffer size %ld, %s\n", buffer_size, snd_strerror(pcm));
        return NULL;
    }
    printf("Audio: Set buffer size to: %ld\n", buffer_size);

    /* Write parameters */
    if ((pcm = snd_pcm_hw_params(pcm_handle, params)) < 0)
    {
        fprintf(stderr, "Audio: Fatal Error: Can't set harware parameters. %s\n", snd_strerror(pcm));
        return NULL;
    }

    printf("Audio: PCM device: '%s'\n", snd_pcm_name(pcm_handle));

    buff_size = periodsize * CHANNELS * sizeof(int16_t);
    buff = (char *) malloc(buff_size);

    snd_pcm_sframes_t avail;
    uint32_t retrieved_samples;
    uint32_t written_samples;
    snd_pcm_sframes_t r;

    //snd_pcm_start(pcm_handle);
    buffer_circular_flush(&buffer_circular_audio);
    
    uint32_t occupied;
    while(!*exit_requested)
    {
        avail = snd_pcm_avail_update(pcm_handle);
        if(avail == -32)
        {
            /* Handle underrun */
            snd_pcm_prepare(pcm_handle);
        }
        while (avail >= (snd_pcm_sframes_t)periodsize)
        {
            buffer_circular_thresholdPop(&buffer_circular_audio, periodsize, periodsize, buff, &retrieved_samples);
            written_samples = 0;
            r = snd_pcm_writei(pcm_handle, buff, retrieved_samples);
            while(r > 0 && (written_samples + r) < retrieved_samples)
            {
                written_samples += r;
                r = snd_pcm_writei(pcm_handle, &buff[written_samples], (retrieved_samples - written_samples));
            }
            if (r == -EPIPE)
            {
                /* Handle underrun */
                printf("Audio: Under-run (XRUN).\n");
                snd_pcm_prepare(pcm_handle);
            }
            else if(r == -EBADFD)
            {
                fprintf(stderr, "Audio Error: PCM not in right state.\n");
            }
            else if(r == ESTRPIPE)
            {
                fprintf(stderr, "Audio Error: Suspend event occurred.\n");
            }

            avail = snd_pcm_avail_update(pcm_handle);
        }

        buffer_circular_stats(&buffer_circular_audio, NULL, NULL, NULL, &occupied);
        if(occupied > 1024)
        {
            printf("Audio: flushing input buffer\n");
            buffer_circular_flush(&buffer_circular_audio);
        }
    }

    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    free(buff);

    return 0;
}