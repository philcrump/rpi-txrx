#ifndef __IF_SUBSAMPLE_H__
#define __IF_SUBSAMPLE_H__

#define IF_FFT_BUFFER_COPY_SIZE 65536

typedef struct {
    uint32_t index;
    uint32_t size;
    char data[IF_FFT_BUFFER_COPY_SIZE * sizeof(float)];
    pthread_mutex_t mutex;
    pthread_cond_t signal;
} if_fft_buffer_t;

void *if_subsample_thread(void *arg);

#endif /* __IF_SUBSAMPLE_H__ */
