#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <alsa/asoundlib.h>

#include "buffer/buffer_circular.h"
#include "timing.h"

#define PCM_DEVICE "default"
#define SAMPLERATE  10549 //5800 //5439
#define CHANNELS    1

/* Samplerate should be 512000 / 50 = 10240 sps
 in csdr example: 2.4Ms/s / 50 = 48000 sps
*/

/* Audio Playback Thread */
void *audio_playback_thread(void *arg)
{
    bool *exit_requested = (bool *)arg;

    int pcm;
    unsigned int tmp;
    snd_pcm_t *pcm_handle;
    snd_pcm_hw_params_t *params;
    snd_pcm_uframes_t period_frames_count;
    char *buff;
    int buff_size;

    unsigned int samplerate = SAMPLERATE;

    /* Open PCM device in playback */
    if ((pcm = snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0)) < 0) 
    {
        fprintf(stderr,"Audio: Error: Can't open \"%s\" PCM device. (%s)\n", PCM_DEVICE, snd_strerror(pcm));
        return NULL;
    }

    /* Allocate parameters object and fill it with default values*/
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_handle, params);

    /* Set parameters */
    if ((pcm = snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
    {
        printf("ERROR: Can't set interleaved mode. %s\n", snd_strerror(pcm));
    }

    if ((pcm = snd_pcm_hw_params_set_format(pcm_handle, params, SND_PCM_FORMAT_S16_LE)) < 0)
    {
        printf("ERROR: Can't set format. %s\n", snd_strerror(pcm));
    }

    if ((pcm = snd_pcm_hw_params_set_channels(pcm_handle, params, CHANNELS)) < 0)
    {
        printf("ERROR: Can't set channels number. %s\n", snd_strerror(pcm));
    }

    if ((pcm = snd_pcm_hw_params_set_rate(pcm_handle, params, samplerate, 0)) < 0) 
    {
        printf("ERROR: Can't set rate %d, %s\n", samplerate, snd_strerror(pcm));
    }

    snd_pcm_uframes_t buffer_size = samplerate / 4;
    if ((pcm = snd_pcm_hw_params_set_buffer_size_near(pcm_handle, params, &buffer_size)) < 0) 
    {
        printf("ERROR: Can't set buffer size %ld, %s\n", buffer_size, snd_strerror(pcm));
    }

    /* Write parameters */
    if ((pcm = snd_pcm_hw_params(pcm_handle, params)) < 0)
    {
        printf("ERROR: Can't set harware parameters. %s\n", snd_strerror(pcm));
    }

    /* Resume information */
    printf("PCM name: '%s'\n", snd_pcm_name(pcm_handle));
    printf("PCM state: %s\n", snd_pcm_state_name(snd_pcm_state(pcm_handle)));

    snd_pcm_hw_params_get_channels(params, &tmp);
    printf("Configured Channels: %i\n", tmp);

    snd_pcm_hw_params_get_rate(params, &tmp, 0);
    printf("rate: %d sps\n", tmp); 

    /* Allocate buffer to hold single period */
    snd_pcm_hw_params_get_period_size(params, &period_frames_count, 0);
    printf("period: %ld samples\n", period_frames_count);

    buff_size = period_frames_count * CHANNELS * sizeof(int16_t);
    buff = (char *) malloc(buff_size);

    snd_pcm_hw_params_get_period_time(params, &tmp, NULL);

    snd_pcm_sframes_t avail;
    uint32_t retrieved_samples;
    uint32_t written_samples;
    snd_pcm_sframes_t r;

    snd_pcm_start(pcm_handle);
    buffer_circular_flush(&buffer_circular_audio);
    
    while(!*exit_requested)
    {
        /* Wait until device is ready for more data */
        if(snd_pcm_wait(pcm_handle, 10))
        {
            avail = snd_pcm_avail_update(pcm_handle);
            if(avail == -32)
            {
                /* Handle underrun */
                snd_pcm_prepare(pcm_handle);
            }
            while (avail >= (snd_pcm_sframes_t)period_frames_count)
            {
                buffer_circular_pop(&buffer_circular_audio, period_frames_count, buff, &retrieved_samples);

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
                    printf("Audio: XRUN.\n");
                    snd_pcm_prepare(pcm_handle);
                    //r = snd_pcm_writei(pcm_handle, buff, retrieved_samples);
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
        }

        uint32_t occupied;
        //uint32_t head, tail, capacity;
        //buffer_circular_stats(&buffer_circular_audio, &head, &tail, &capacity, &occupied);
        //printf("Audio Buffer: Head: %d, Tail: %d, Capacity: %d, Occupied: %d\n",
        //    head, tail, capacity, occupied
        //);
        buffer_circular_stats(&buffer_circular_audio, NULL, NULL, NULL, &occupied);
        if(occupied > 1024)
        {
            printf("flush\n");
            buffer_circular_flush(&buffer_circular_audio);
        }
    }

    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    free(buff);

    return 0;
}