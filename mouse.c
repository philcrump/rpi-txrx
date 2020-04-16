#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include <linux/input.h>
#include <fcntl.h>

#include "timing.h"
#include "graphics.h"

/* Hardcoded, but is only one in 'by-id' directory so could use first entry in this dir? */
char *mouse_event_path = "/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-mouse";

/* Hz per click */
#define SCROLL_SPEED_HIGH   5000
#define SCROLL_SPEED_MEDIUM 500
#define SCROLL_SPEED_SLOW   50

/* Max ms between clicks */
#define SCROLL_TIME_HIGH   30 
#define SCROLL_TIME_MEDIUM 80

extern int64_t center_frequency;
extern int64_t span_frequency;

extern int64_t selected_center_frequency;
extern int64_t selected_span_frequency;

/* Mouse Thread */
void *mouse_thread(void *arg)
{
    bool *exit_requested = (bool *)arg;

    int fd;
    struct input_event event_data;

    if((fd = open(mouse_event_path, O_RDONLY)) == -1)
    {
        printf("Mouse: Error opening event device\n");
        return NULL;
    }

    int64_t distance_from_limit;
    uint64_t last_scroll_monotonic = 0;

    graphics_frequency_newdata();

    while(false == *exit_requested)
    {
        if(read(fd, &event_data, sizeof(event_data)))
        {
            if ((event_data.code == 8) && (event_data.type == 2) &&
            ((event_data.value == 1) || (event_data.value == -1)))
            {
                if(event_data.value == 1)
                {
                    /* Upwards */
                    distance_from_limit = (center_frequency + (span_frequency / 2)) - selected_center_frequency;

                    if(distance_from_limit <= 0)
                    {
                        continue;
                    }

                    if(monotonic_ms() - last_scroll_monotonic < SCROLL_TIME_HIGH)
                    {
                        /* Fast scroll */
                        if(distance_from_limit < SCROLL_SPEED_HIGH)
                        {
                            selected_center_frequency += distance_from_limit;
                        }
                        else
                        {
                            selected_center_frequency += SCROLL_SPEED_HIGH;
                        }
                    }
                    else if(monotonic_ms() - last_scroll_monotonic < SCROLL_TIME_MEDIUM)
                    {
                        /* Medium scroll */
                        if(distance_from_limit < SCROLL_SPEED_MEDIUM)
                        {
                            selected_center_frequency += distance_from_limit;
                        }
                        else
                        {
                            selected_center_frequency += SCROLL_SPEED_MEDIUM;
                        }
                    }
                    else
                    {
                        /* Slow scroll */
                        if(distance_from_limit < SCROLL_SPEED_SLOW)
                        {
                            selected_center_frequency += distance_from_limit;
                        }
                        else
                        {
                            selected_center_frequency += SCROLL_SPEED_SLOW;
                        }
                    }
                    last_scroll_monotonic = monotonic_ms();
                    graphics_frequency_newdata();
                }
                else if(event_data.value == -1)
                {
                    /* Downwards */
                    distance_from_limit = selected_center_frequency - (center_frequency - (span_frequency / 2));

                    if(distance_from_limit <= 0)
                    {
                        continue;
                    }

                    if(monotonic_ms() - last_scroll_monotonic < SCROLL_TIME_HIGH)
                    {
                        /* Fast scroll */
                        if(distance_from_limit < SCROLL_SPEED_HIGH)
                        {
                            selected_center_frequency -= distance_from_limit;
                        }
                        else
                        {
                            selected_center_frequency -= SCROLL_SPEED_HIGH;
                        }
                    }
                    else if(monotonic_ms() - last_scroll_monotonic < SCROLL_TIME_MEDIUM)
                    {
                        /* Medium scroll */
                        if(distance_from_limit < SCROLL_SPEED_MEDIUM)
                        {
                            selected_center_frequency -= distance_from_limit;
                        }
                        else
                        {
                            selected_center_frequency -= SCROLL_SPEED_MEDIUM;
                        }
                    }
                    else
                    {
                        /* Slow scroll */
                        if(distance_from_limit < SCROLL_SPEED_SLOW)
                        {
                            selected_center_frequency -= distance_from_limit;
                        }
                        else
                        {
                            selected_center_frequency -= SCROLL_SPEED_SLOW;
                        }
                    }
                    last_scroll_monotonic = monotonic_ms();
                    graphics_frequency_newdata();
                }
            }
#if 1
            printf("time %ld.%06ld: Value=%d\n",
                event_data.time.tv_sec, event_data.time.tv_usec,
                event_data.value
            );
#endif
        }
    }

    close(fd);

    return NULL;
}