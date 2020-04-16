/*
    Several functions in this file have been heavily derived, or directly copied, from libcsdr.

    Copyright (c) Andras Retzler, HA7ILM <randras@sdr.hu>
    Copyright (c) Warren Pratt, NR0V <warren@wpratt.com>
    Copyright 2006,2010,2012 Free Software Foundation, Inc.

    libcsdr is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    libcsdr is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with libcsdr.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <limits.h>
#include <fftw3.h>

#include "timing.h"
#include "if_demod.h"
#include "buffer/buffer_circular.h"

extern int64_t center_frequency;
extern int64_t selected_center_frequency;

/* Demod configuration vars */
float low_cut = 0.02; // ~100Hz
float high_cut = 0.3;
float transition_bw = 0.1; // 0.05


/* Demod Internal Vars */
static int fft_size;
static int input_size;
static int overlap_length;

static fftwf_plan demod_plan_taps;
static buffer_iqsample_t* taps_fft;

static buffer_iqsample_t* input;
static fftwf_plan demod_plan_forward;
static buffer_iqsample_t* input_fourier;

static buffer_iqsample_t* output_fourier;
static fftwf_plan demod_plan_inverse_1;
static buffer_iqsample_t* output_1;
static fftwf_plan demod_plan_inverse_2;
static buffer_iqsample_t* output_2;

/* Sanity check that these types are interchangeable */
_Static_assert(sizeof(buffer_iqsample_t) == sizeof(fftwf_complex), "Error: sizeof(buffer_iqsample_t) == sizeof(fftwf_complex) failed!");

static float window_kernel_hamming(float rate)
{
    //Explanation at Chapter 16 of dspguide.com, page 2
    //Hamming window has worse stopband attentuation and passband ripple than Blackman, but it has faster rolloff.
    rate=0.5+rate/2;
    return 0.54-0.46*cos(2*M_PI*rate);
}

static void firdes_lowpass_f(float *output, int length, float cutoff_rate)
{   //Generates symmetric windowed sinc FIR filter real taps
    //  length should be odd
    //  cutoff_rate is (cutoff frequency/sampling frequency)
    //Explanation at Chapter 16 of dspguide.com
    int middle=length/2;
    output[middle]=2*M_PI*cutoff_rate*window_kernel_hamming(0);
    for(int i=1; i<=middle; i++) //@@firdes_lowpass_f: calculate taps
    {
        output[middle-i]=output[middle+i]=(sin(2*M_PI*cutoff_rate*i)/i)*window_kernel_hamming((float)i/middle);
        //printf("%g %d %d %d %d | %g\n",output[middle-i],i,middle,middle+i,middle-i,sin(2*PI*cutoff_rate*i));
    }
    //Normalize filter kernel
    float sum=0;
    for(int i=0;i<length;i++) //@normalize_fir_f: normalize pass 1
        sum+=output[i];
    for(int i=0;i<length;i++) //@normalize_fir_f: normalize pass 2
        output[i]=output[i]/sum;
}

static float agc_ff(float* input, float* output, int input_size, float reference, float attack_rate, float decay_rate, float max_gain, short hang_time, short attack_wait_time, float gain_filter_alpha, float last_gain)
{
    /*
        Notes on parameters (with some default values):
            attack_rate = 0.01
            decay_rate = 0.001
            hang_time = (hang_time_ms / 1000) * sample_rate
                hang_time is given in samples, and should be about 4ms.
                hang_time can be switched off by setting it to zero (not recommended).
            max_gain = pow(2, adc_bits)
                max_gain should be no more than the dynamic range of your A/D converter.
            gain_filter_alpha = 1 / ((fs/(2*PI*fc))+1)
            >>> 1 / ((48000./(2*3.141592654*100))+1)
            0.012920836043344543
            >>> 1 / ((48000./(2*3.141592654*10))+1)
            0.0013072857061786625
        Literature:
            ww.qsl.net/va3iul/Files/Automatic_Gain_Control.pdf
            page 7 of http://www.arrl.org/files/file/Technology/tis/info/pdf/021112qex027.pdf
        Examples:
            http://svn.tapr.org/repos_sdr_hpsdr/trunk/W5WC/PowerSDR_HPSDR_mRX_PS/Source/wdsp/wcpAGC.c
            GNU Radio's agc,agc2,agc3 have quite good ideas about this.
    */
    register short hang_counter=0;
    register short attack_wait_counter=0;
    float gain=last_gain;
    float last_peak=reference/last_gain; //approx.
    float input_abs;
    float error, dgain;
    output[0]=last_gain*input[0]; //we skip this one sample, because it is easier this way
    for(int i=1;i<input_size;i++) //@agc_ff
    {
        //printf("[%d] %f, ", i, input[i]);
        //The error is the difference between the required gain at the actual sample, and the previous gain value.
        //We actually use an envelope detector.
        input_abs=fabs(input[i]);
        error=reference/input_abs-gain;

        if(input[i]!=0) //We skip samples containing 0, as the gain would be infinity for those to keep up with the reference.
        {
            //An AGC is something nonlinear that's easier to implement in software:
            //if the amplitude decreases, we increase the gain by minimizing the gain error by attack_rate.
            //We also have a decay_rate that comes into consideration when the amplitude increases.
            //The higher these rates are, the faster is the response of the AGC to amplitude changes.
            //However, attack_rate should be higher than the decay_rate as we want to avoid clipping signals.
            //that had a sudden increase in their amplitude.
            //It's also important to note that this algorithm has an exponential gain ramp.

            if(error<0) //INCREASE IN SIGNAL LEVEL
            {
                if(last_peak<input_abs)
                {

                    attack_wait_counter=attack_wait_time;
                    last_peak=input_abs;
                }
                if(attack_wait_counter>0)
                {
                    attack_wait_counter--;
                    //fprintf(stderr,"A");
                    dgain=0;
                }
                else
                {
                    //If the signal level increases, we decrease the gain quite fast.
                    dgain=error*attack_rate;
                    //Before starting to increase the gain next time, we will be waiting until hang_time for sure.
                    hang_counter=hang_time;

                }
            }
            else //DECREASE IN SIGNAL LEVEL
            {
                if(hang_counter>0) //Before starting to increase the gain, we will be waiting until hang_time.
                {
                    hang_counter--;
                    dgain=0; //..until then, AGC is inactive and gain doesn't change.
                }
                else dgain=error*decay_rate; //If the signal level decreases, we increase the gain quite slowly.
            }
            gain=gain+dgain;
            //fprintf(stderr,"g=%f dg=%f\n",gain,dgain);
        }
        if(gain>max_gain) gain=max_gain; //We also have to limit our gain, it can't be infinity.
        if(gain<0) gain=0;
        //output[i]=gain*input[i]; //Here we do the actual scaling of the samples.
        //Here we do the actual scaling of the samples, but we run an IIR filter on the gain values:
        output[i]=(gain=gain+last_gain-gain_filter_alpha*last_gain)*input[i]; //dc-pass-filter: freqz([1 -1],[1 -0.99]) y[i]=x[i]+y[i-1]-alpha*x[i-1]
        //output[i]=input[i]*(last_gain+gain_filter_alpha*(gain-last_gain)); //LPF

        last_gain=gain;
    }
    return gain; //this will be the last_gain next time
}

// csdr bandpass_fir_fft_cc 0 0.1 0.05

void if_demod_init(void)
{
    /* Calculate FFT filter length (number of non-zero taps) */
    int taps_length = 4.0 / transition_bw;
    if(taps_length % 2 == 0) taps_length++; //number of symmetric FIR filter taps should be odd

    //int fft_size;
    for(int i = 0; i < 31; i++)
    {
        if(taps_length < (fft_size = 1 << i)) break;
    }
    //the number of padding zeros is the number of output samples we will be able to take away after every processing step, and it looks sane to check if it is large enough.
    if((fft_size - taps_length) < 200) fft_size <<= 1;

    input_size = fft_size - taps_length + 1;
    overlap_length = taps_length - 1;
    printf("IF Demod: (fft_size = %d) = (taps_length = %d) + (input_size = %d) - 1 (overlap_length = %d) = taps_length - 1\n", fft_size, taps_length, input_size, overlap_length );
    if(fft_size <= 2)
    {
        fprintf(stderr,"IF Demod: FFT size error. (fft_size <= 2)");
        return;
    }

    //prepare making the filter and doing FFT on it
    buffer_iqsample_t* taps = (buffer_iqsample_t*)calloc(sizeof(buffer_iqsample_t), fft_size); //calloc initializes to zero
    taps_fft = (buffer_iqsample_t*)malloc(sizeof(buffer_iqsample_t) * fft_size);

    demod_plan_taps = fftwf_plan_dft_1d(fft_size, (fftwf_complex*)taps, (fftwf_complex*)taps_fft, FFTW_FORWARD, FFTW_PATIENT);
    printf(" "); fftwf_print_plan(demod_plan_taps); printf("\n");

    //make FFT plans for continously processing the input
    input = fftwf_malloc(fft_size*sizeof(buffer_iqsample_t));
    input_fourier = fftwf_malloc(fft_size*sizeof(buffer_iqsample_t));
    demod_plan_forward = fftwf_plan_dft_1d(fft_size, (fftwf_complex*)input, (fftwf_complex*)input_fourier, FFTW_FORWARD, FFTW_PATIENT);
    printf(" "); fftwf_print_plan(demod_plan_forward); printf("\n");

    output_fourier = fftwf_malloc(fft_size*sizeof(buffer_iqsample_t));
    output_1 = fftwf_malloc(fft_size*sizeof(buffer_iqsample_t));
    output_2 = fftwf_malloc(fft_size*sizeof(buffer_iqsample_t));
    //we create 2x output buffers so that one will preserve the previous overlap:
    demod_plan_inverse_1 = fftwf_plan_dft_1d(fft_size, (fftwf_complex*)output_fourier, (fftwf_complex*)output_1, FFTW_BACKWARD, FFTW_PATIENT);
    demod_plan_inverse_2 = fftwf_plan_dft_1d(fft_size, (fftwf_complex*)output_fourier, (fftwf_complex*)output_2, FFTW_BACKWARD, FFTW_PATIENT);
    printf(" "); fftwf_print_plan(demod_plan_inverse_2); printf("\n");

    //we initialize the second output buffer to 0 as it will be taken as the overlap source for the first time:
    for(int i = 0; i < fft_size; i++) output_2[i].i = output_2[i].q = 0;

    //we pre-pad the input buffer with zeros
    for(int i = input_size; i < fft_size; i++) input[i].i = input[i].q = 0;

    /** make the filter (from here down needs to be re-run if low_cut or high_cut is changed) **/
    //printf("IF Demod: filter initialising, low_cut = %g, high_cut = %g\n", low_cut, high_cut);

    /* firdes_bandpass_c() */
    //To generate a complex filter:
    //  1. we generate a real lowpass filter with a bandwidth of highcut-lowcut
    //  2. we shift the filter taps spectrally by multiplying with e^(j*w), so we get complex taps
    //(tnx HA5FT)
    float* realtaps = (float*)malloc(taps_length * sizeof(float));

    firdes_lowpass_f(realtaps, taps_length, (high_cut - low_cut) / 2);
    float filter_center = (high_cut + low_cut) / 2;

    float phase=0, sinval, cosval;
    for(int i=0; i<taps_length; i++) //@@firdes_bandpass_c
    {
        cosval = cos(phase);
        sinval = sin(phase);
        phase += 2 * M_PI * filter_center;
        while(phase > (2 * M_PI)) phase -= 2 * M_PI; //@@firdes_bandpass_c
        while(phase < 0) phase += 2 * M_PI;
        taps[i].i = cosval * realtaps[i];
        taps[i].q = sinval * realtaps[i];
    }

    fftwf_execute(demod_plan_taps);
}

/* IF Demodulator Thread */
void *if_demod_thread(void *arg)
{
    bool *exit_requested = (bool *)arg;

    uint32_t length_returned;

    /* FIR FFT */
    bool odd = false;
    int i;
    buffer_iqsample_t* last_overlap;
    buffer_iqsample_t* result;

    /* agc_ff */
    short hang_time=200;
    float reference=0.2;
    float attack_rate=0.01;
    float decay_rate=0.0001;
    float max_gain=65536;
    short attack_wait=0;
    float filter_alpha=0.999;

    float last_gain=1.0;


    float *realpart_buffer = (float *)malloc(input_size * sizeof(float));
    float *agc_buffer = (float *)malloc(input_size * sizeof(float));
    int16_t *s16_buffer = (int16_t *)malloc(input_size * sizeof(int16_t));


#if 0
    bool monotonic_started = false;
    uint64_t samples_total = 0;
    uint64_t start_monotonic = 0;
#endif

    while(!*exit_requested)
    {
        /* Wait for incoming data */
        length_returned = 0;
        buffer_circular_waitThresholdPop(&buffer_circular_iq_if, input_size, input_size, input, &length_returned);
        if(length_returned != (uint32_t)input_size)
        {
            fprintf(stderr, "demod error: input buffer_circular returned wrong length\n");
            continue;
        }

#if 0
        if(!monotonic_started)
        {
            start_monotonic = monotonic_ms();
            monotonic_started = true;
        }
#endif

        last_overlap = (buffer_iqsample_t*)((odd) ? output_1 : output_2) + input_size; //+ fft_size - overlap_length;

        /** use the overlap & add method for filtering **/

        //calculate FFT on input buffer
        fftwf_execute(demod_plan_forward);

        //multiply the filter and the input
        for(i = 0; i < fft_size; i++) //@apply_fir_fft_cc: multiplication
        {
            output_fourier[i].i = (input_fourier[i].i * taps_fft[i].i) - (input_fourier[i].q * taps_fft[i].q);
            output_fourier[i].q = (input_fourier[i].i * taps_fft[i].q) + (input_fourier[i].q * taps_fft[i].i);
        }

        //calculate inverse FFT on multiplied buffer
        fftwf_execute((odd)?demod_plan_inverse_2:demod_plan_inverse_1);

        //add the overlap of the previous segment
        result = (odd) ? output_2 : output_1;

        for(i = 0; i < fft_size; i++) //@apply_fir_fft_cc: normalize by fft_size
        {
            result[i].i /= fft_size;
            result[i].q /= fft_size;
        }

        for(i = 0; i < overlap_length; i++) //@apply_fir_fft_cc: add overlap
        {
            result[i].i += last_overlap[i].i;
            result[i].q += last_overlap[i].q;
        }
        odd = !odd;

#if 0
        printf("Demod finished: %d samples\n", input_size);
        for(i = 0; i < input_size; i++)
        {
            printf("%f,%f,", result[i].i, result[i].q);
        }
        printf("\n");
#endif

        // csdr realpart_cf
        // also check for NaN and copy previous converted result if isnan() (avoids clicks)
        for(i = 0; i < input_size; i++)
        {
            realpart_buffer[i] = isnan(result[i].i) ? (i > 0 ? realpart_buffer[i-1] : 0.0) : result[i].i;
        }

        // csdr agc_ff
        last_gain = agc_ff(realpart_buffer, agc_buffer, input_size, reference, attack_rate, decay_rate, max_gain, hang_time, attack_wait, filter_alpha, last_gain);

        // csdr limit_ff (done in place)
        for (i = 0; i < input_size; i++)
        {
            agc_buffer[i] = (1.0 < agc_buffer[i]) ? 1.0 : agc_buffer[i];
            agc_buffer[i] = (-1.0 > agc_buffer[i] ) ? -1.0 : agc_buffer[i];
        }

        // csdr convert_f_s16
        for(i = 0; i < input_size; i++)
        {
            s16_buffer[i] = agc_buffer[i] * SHRT_MAX; //@convert_f_s16
        }

#if 0
        printf("Demod finished: %d samples\n", input_size);
        for(i = 0; i < input_size; i++)
        {
            printf("%d,", s16_buffer[i]);
        }
        printf("\n");
#endif


#if 0
        samples_total += input_size;
        printf("Audio samplerate: %.3f\n", (float)(samples_total * 1000) / (monotonic_ms() - start_monotonic));
#endif
        // Samplerate appears to be approximately 10550Hz


        /* Copy out for audio playback */
        uint32_t samples_rx_transferred = input_size;
        buffer_circular_push(&buffer_circular_audio, s16_buffer, &samples_rx_transferred);
        if(samples_rx_transferred > 0)
        {
            fprintf(stderr, "IQ Demod: WARNING push to audio buffer was lossy (%d / %d returned)\n",
                samples_rx_transferred, input_size);
        }

#if 0
        uint32_t head, tail, capacity, occupied;
        buffer_circular_stats(&buffer_circular_audio, &head, &tail, &capacity, &occupied);
        printf("Audio Buffer: Head: %d, Tail: %d, Capacity: %d, Occupied: %d\n",
            head, tail, capacity, occupied);
#endif

    }

    return NULL;
}