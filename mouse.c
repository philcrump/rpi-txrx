#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>

#include <linux/input.h>
#include <fcntl.h>
#include <errno.h>

#include "timing.h"
#include "graphics.h"

/* Hardcoded, but is only one in 'by-id' directory so could use first entry in this dir? */
char *mouse_event_path = "/dev/input/mice";

static const uint8_t mouse_intellimouse_command[] = { 0xf3, 200, 0xf3, 100, 0xf3, 80 };
static const int32_t mouse_intellimouse_command_length = 6;

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
    uint8_t mouse_buffer[4];
    ssize_t mouse_buffer_length;

    if((fd = open(mouse_event_path, O_RDWR | O_NOCTTY)) == -1)
    {
        printf("Mouse: Error opening event device\n");
        return NULL;
    }

    /* Switch mouse device to IntelliMouse protocol to get wheel events */
    if(write(fd, mouse_intellimouse_command, mouse_intellimouse_command_length) != mouse_intellimouse_command_length)
    {
        printf("Mouse: Error sending IntelliMouse command\n");
        close(fd);
        return NULL;
    }
    if (read(fd, mouse_buffer, sizeof(mouse_buffer)) != 1 || mouse_buffer[0] != 0xFA)
    {

        printf("Mouse: Error receiving ACK for IntelliMouse command\n");
        close(fd);
        return NULL;
    }


    int64_t distance_from_limit;
    uint64_t last_scroll_monotonic = 0;

    graphics_frequency_newdata();

    while(false == *exit_requested)
    {
        mouse_buffer_length = read(fd, mouse_buffer, 4);
        if(mouse_buffer_length == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            printf("Mouse: Read Error: %s.\n", strerror(errno));
            break;
        }
        else if (mouse_buffer_length != 4 || !(mouse_buffer[0] & 0x08))
        {
            /* Not a 4-byte IntelliMouse Report */
            continue;
        }
        else if(mouse_buffer[3] == 0)
        {
            /* No wheel event here */
            continue;
        }

        if(mouse_buffer[3] == 255)
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
        else if(mouse_buffer[3] == 1)
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

    close(fd);

    return NULL;
}