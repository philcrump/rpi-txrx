/*
    Several functions in this file have been heavily derived from libcsdr.

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

#include "timing.h"
#include "if_subsample.h"
#include "buffer/buffer_circular.h"

if_fft_buffer_t if_fft_buffer;

extern int64_t center_frequency;
extern int64_t selected_center_frequency;

#define INPUT_SIZE      16384

#define DECIMATION_FACTOR   50 // 512 KHz / 50 = 10.240 KHz

void shift_addition_cc(buffer_iqsample_t *input, buffer_iqsample_t* output, float rate, float *starting_phase)
{
    //The original idea was taken from wdsp:
    //http://svn.tapr.org/repos_sdr_hpsdr/trunk/W5WC/PowerSDR_HPSDR_mRX_PS/Source/wdsp/shift.c

    //However, this method introduces noise (from floating point rounding errors), which increases until the end of the buffer.
    //fprintf(stderr, "cosd=%g sind=%g\n", d.cosdelta, d.sindelta);

    float sindelta = sin(2 * rate * M_PI);
    float cosdelta = cos(2 * rate * M_PI);

    float cosphi = cos(*starting_phase);
    float sinphi = sin(*starting_phase);
    float cosphi_last, sinphi_last;

    for(int i = 0; i < INPUT_SIZE; i++) //@shift_addition_cc: work
    {
        output[i].i = (cosphi * input[i].i) - (sinphi * input[i].q);
        output[i].q = (sinphi * input[i].i) + (cosphi * input[i].q);
        //using the trigonometric addition formulas
        //cos(phi+delta)=cos(phi)cos(delta)-sin(phi)*sin(delta)
        cosphi_last = cosphi;
        sinphi_last = sinphi;
        cosphi = (cosphi_last * cosdelta) - (sinphi_last * sindelta);
        sinphi = (sinphi_last * cosdelta) + (cosphi_last * sindelta);
    }

    *starting_phase += rate * M_PI * INPUT_SIZE;
    while(*starting_phase > M_PI) *starting_phase -= 2 * M_PI; //@shift_addition_cc: normalize starting_phase
    while(*starting_phase < -M_PI) *starting_phase += 2 * M_PI;
}

int fir_decimate_cc(buffer_iqsample_t *input, buffer_iqsample_t *output, int buffer_size, int decimation, float *taps, int taps_length)
{
    //Theory: http://www.dspguru.com/dsp/faqs/multirate/decimation
    //It uses real taps. It returns the number of output samples actually written.
    //It needs overlapping input based on its returned value:
    //number of processed input samples = returned value * decimation factor
    //The output buffer should be at least input_length / 3.
    // i: input index | ti: tap index | oi: output index
    int oi=0;
    for(int i=0; i<buffer_size; i+=decimation) //@fir_decimate_cc: outer loop
    {
        if(i+taps_length>buffer_size) break;
        register float* pinput=(float*)&(input[i]);
        register float* ptaps=taps;
        register float* ptaps_end=taps+taps_length;
        float quad_acciq [8];

/*
q0, q1: input signal I sample and Q sample
q2:     taps
q4, q5: accumulator for I branch and Q branch (will be the output)
*/

        asm volatile(
            "       veor q4, q4\n\t"
            "       veor q5, q5\n\t"
            "for_fdccasm%=: vld2.32   {q0-q1}, [%[pinput]]!\n\t" //load q0 and q1 directly from the memory address stored in pinput, with interleaving (so that we get the I samples in q0 and the Q samples in q1), also increment the memory address in pinput (hence the "!" mark) //http://community.arm.com/groups/processors/blog/2010/03/17/coding-for-neon--part-1-load-and-stores
            "       vld1.32 {q2}, [%[ptaps]]!\n\t"
            "       vmla.f32 q4, q0, q2\n\t" //quad_acc_i += quad_input_i * quad_taps_1 //http://stackoverflow.com/questions/3240440/how-to-use-the-multiply-and-accumulate-intrinsics-in-arm-cortex-a8 //http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0489e/CIHEJBIE.html
            "       vmla.f32 q5, q1, q2\n\t" //quad_acc_q += quad_input_q * quad_taps_1
            "       cmp %[ptaps], %[ptaps_end]\n\t" //if(ptaps != ptaps_end)
            "       bcc for_fdccasm%=\n\t"            //  then goto for_fdcasm
            "       vst1.32 {q4}, [%[quad_acci]]\n\t" //if the loop is finished, store the two accumulators in memory
            "       vst1.32 {q5}, [%[quad_accq]]\n\t"
        :
            [pinput]"+r"(pinput), [ptaps]"+r"(ptaps) //output operand list
        :
            [ptaps_end]"r"(ptaps_end), [quad_acci]"r"(quad_acciq), [quad_accq]"r"(quad_acciq+4) //input operand list
        :
            "memory", "q0", "q1", "q2", "q4", "q5", "cc" //clobber list
        );
        //original for loops for reference:
        //for(int ti=0; ti<taps_length; ti++) acci += (iof(input,i+ti)) * taps[ti]; //@fir_decimate_cc: i loop
        //for(int ti=0; ti<taps_length; ti++) accq += (qof(input,i+ti)) * taps[ti]; //@fir_decimate_cc: q loop

        //for(int n=0;n<8;n++) fprintf(stderr, "\n>> [%d] %g \n", n, quad_acciq[n]);
        output[oi].i = quad_acciq[0] + quad_acciq[1] + quad_acciq[2] + quad_acciq[3]; //we're still not ready, as we have to add up the contents of a quad accumulator register to get a single accumulated value
        output[oi].q = quad_acciq[4] + quad_acciq[5] + quad_acciq[6] + quad_acciq[7];
        oi++;
    }

    return oi;
}

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

/* IF Subsample Thread */
void *if_subsample_thread(void *arg)
{
    bool *exit_requested = (bool *)arg;

    /** Shift Addition Prep **/

    float shift_addition_cc_rate;
    float shift_addition_cc_phase = 0.0;

    /** decimation Prep **/
    float transition_bw = 0.05;

    int taps_length = 4.0 / transition_bw;
    if(taps_length % 2 == 0) taps_length++; //number of symmetric FIR filter taps should be odd


    #define NEON_ALIGNMENT (4*4*2)

    int padded_taps_length = taps_length + (NEON_ALIGNMENT / 4) - 1 - ((taps_length + (NEON_ALIGNMENT / 4) - 1) % (NEON_ALIGNMENT / 4));
    //printf("if_subsample: padded_taps_length = %d (from %d)\n", padded_taps_length, taps_length);
    
    float *taps;
    taps = (float*)malloc((padded_taps_length + NEON_ALIGNMENT) * sizeof(float));
    //fprintf(stderr,"taps = %p\n", (void *)taps);
    taps =  (float*)((((unsigned)taps) + NEON_ALIGNMENT - 1) & ~(NEON_ALIGNMENT - 1));
    //fprintf(stderr,"NEON aligned taps = %p\n", (void *)taps);
    for(int i = 0; i < (padded_taps_length - taps_length); i++) taps[taps_length + i] = 0;

    /* Hardcoded for Hamming window */
    firdes_lowpass_f(taps, taps_length, 0.5 / (float)DECIMATION_FACTOR);

    /** Buffers! **/

    buffer_iqsample_t *buffer_1 = (buffer_iqsample_t *)malloc(sizeof(buffer_iqsample_t) * (INPUT_SIZE + padded_taps_length));
    buffer_iqsample_t *buffer_2 = (buffer_iqsample_t *)malloc(sizeof(buffer_iqsample_t) * INPUT_SIZE);
    buffer_iqsample_t *buffer_3 = (buffer_iqsample_t *)malloc(sizeof(buffer_iqsample_t) * (INPUT_SIZE / DECIMATION_FACTOR));

    if(buffer_1 == NULL || buffer_2 == NULL || buffer_3 == NULL)
    {
        fprintf(stderr, "Error: Failed to allocated buffers for if_subsample, aborting.\n");
        /* TODO, Free allocated buffers here */
        return NULL;
    }

#if 0
    bool monotonic_started = false;
    uint64_t samples_total = 0, decimated_samples_total = 0;
    uint64_t start_monotonic = 0;
#endif

    uint32_t buffer_length_returned;

    int subsample_output_samples = 0;
    int overlap = 0;
    while(!*exit_requested)
    {
        /* Wait for incoming data */
        buffer_length_returned = 0;
        buffer_circular_waitThresholdPop(&buffer_circular_iq_main, INPUT_SIZE, INPUT_SIZE, buffer_1, &buffer_length_returned);
        if(buffer_length_returned != INPUT_SIZE)
        {
            fprintf(stderr, "Subsample Error: subsample input buffer returned incorrect length\n");
            continue;
        }

#if 0
        if(!monotonic_started)
        {
            start_monotonic = monotonic_ms();
            monotonic_started = true;
        }
#endif

        /* Prepare current frequency values */
        shift_addition_cc_rate = (float)(center_frequency - selected_center_frequency) / 512000.0;

        /* Shift it */
        shift_addition_cc(buffer_1, &buffer_2[overlap], shift_addition_cc_rate, &shift_addition_cc_phase);
    
        /* Subsample it */
        subsample_output_samples = fir_decimate_cc(buffer_2, buffer_3, INPUT_SIZE + overlap, DECIMATION_FACTOR, taps, padded_taps_length);
        //printf("SUB: returned %d samples / 16384 (overlap: %d, ptapslength: %d)\n", subsample_output_samples, overlap, padded_taps_length);
        /* Shift over for overlap in next iteration */
        overlap = (INPUT_SIZE + overlap) - (subsample_output_samples * DECIMATION_FACTOR);

#if 0
        /**** DEBUG ****/
        samples_total += INPUT_SIZE;
        decimated_samples_total += subsample_output_samples;
        printf("Subsample samplerate: %.3f (total: %lld) -> %.3f\n",
            (float)(samples_total * 1000) / (monotonic_ms() - start_monotonic), samples_total,
            (float)(decimated_samples_total * 1000) / (monotonic_ms() - start_monotonic)
        );
#endif

        /* Copy out for IF FFT */
        pthread_mutex_lock(&if_fft_buffer.mutex);
        /* Reset index so IF FFT knows it's new data */
        if_fft_buffer.index = 0;
        memcpy(
            if_fft_buffer.data,
            buffer_3,
            (subsample_output_samples * sizeof(buffer_iqsample_t))
        );
        if_fft_buffer.size = (subsample_output_samples * sizeof(buffer_iqsample_t));
        pthread_cond_signal(&if_fft_buffer.signal);
        pthread_mutex_unlock(&if_fft_buffer.mutex);

        /* Copy out for IF demod */
        uint32_t samples_rx_transferred = subsample_output_samples;
        buffer_circular_push(&buffer_circular_iq_if, (buffer_iqsample_t *)buffer_3, &samples_rx_transferred);
        if(samples_rx_transferred > 0)
        {
            fprintf(stderr, "IQ Subsample: WARNING push to IF demod buffer was lossy (%d / %d returned)\n",
                samples_rx_transferred, subsample_output_samples);
        }
    }

    free(buffer_1);
    free(buffer_2);
    free(buffer_3);

    return NULL;
}