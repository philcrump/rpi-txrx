#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <dirent.h>

#include "screen.h"
#include "graphics.h"
#include "timing.h"

bool ptt_pressed = false;

static bool touch_detecthw(char **touchscreen_path_ptr)
{
  FILE * fp;
  char * ln=NULL;
  size_t len=0;
  ssize_t rd;
  int p;
  char handler[10];
  char * found;
  p=0;

  fp=fopen("/proc/bus/input/devices","r");

  while ((rd=getline(&ln,&len,fp)!=-1))
  {
    if(ln[0]=='N')        //name of device
    {
      if(strstr(ln,"FT5406")!=NULL) p=1; else p=0;     //Found Raspberry Pi TouchScreen entry
    }

    if ((p==1) && (ln[0]=='H'))        // found handler
    {
      if(strstr(ln,"mouse")!=NULL)
      {
        found=strstr(ln,"event");
        strcpy(handler,found);
        handler[6]=0;
        asprintf(touchscreen_path_ptr, "/dev/input/%s", handler);
        fclose(fp);
        free(ln);
        return true;
      }
    }   
  }

  fclose(fp);
  if(ln)  free(ln);
  return false;
}

#define TOUCH_EVENT_START 1
#define TOUCH_EVENT_END   2
#define TOUCH_EVENT_MOVE   3

//Returns 0 if no touch available. 1 if Touch start. 2 if touch end. 3 if touch move
static void touch_readEvents(int touch_fd, void (*touch_callback)(int type, int x, int y))
{
  size_t i, rb;
  struct input_event ev[64];
  int retval;
  int touch_type = -1, touch_x = 0, touch_y = 0;

  retval = -1;

  rb=read(touch_fd, ev, sizeof(struct input_event)*64);

  for (i = 0;  i <  (rb / sizeof(struct input_event)); i++)
  {
    if (ev[i].type ==  EV_SYN) 
    {
      if(retval == -1)
      {
        if(touch_type == TOUCH_EVENT_START)
        {
            /* This concludes a START event, [START, X, Y, MOVE] */
            touch_callback(touch_type, touch_x, touch_y);
        }
        else if(touch_type == TOUCH_EVENT_END)
        {
            /* This concludes a END event, [END, MOVE] */
            touch_callback(touch_type, 0, 0);
        }
        else
        {
            /* Concludes a MOVE EVENT [<X>, <Y>, MOVE] */
            touch_type = TOUCH_EVENT_MOVE;
            touch_callback(touch_type, touch_x, touch_y);
        }
      }
    }
    else if (ev[i].type == EV_KEY && ev[i].code == 330 && ev[i].value == 1)
    {
      touch_type = TOUCH_EVENT_START;
    }
    else if (ev[i].type == EV_KEY && ev[i].code == 330 && ev[i].value == 0)
    {
      touch_type = TOUCH_EVENT_END;
    }
    else if (ev[i].type == EV_ABS && ev[i].code == 0 && ev[i].value > 0)
    {
      touch_x = ev[i].value;
    }
    else if (ev[i].type == EV_ABS  && ev[i].code == 1 && ev[i].value > 0)
    {
      touch_y = ev[i].value;
    }
  }
}

#define INOTIFY_FD_BUFFER_LENGTH        (64 * (sizeof(struct inotify_event) + NAME_MAX + 1))

static void touch_run(char *touch_path, void (*touch_callback)(int type, int x, int y))
{
  int inotify_fd, r;
  int touch_fd;

  /* Check that file_path exists */
  struct stat file_st;
  if(stat(touch_path, &file_st) < 0)
  {
    fprintf(stderr, "inotify: File path does not exist: %s\n", touch_path);
    return;
  }

  /* Set up inotify */
  inotify_fd = inotify_init();
  if(inotify_fd == -1)
  {
    fprintf(stderr, "inotify: inotify_init() returned error: %s\n", strerror(inotify_fd));
    return;
  }

  /* Start watching file for changes */
  r = inotify_add_watch(inotify_fd, touch_path, IN_ACCESS);
  if(r < 0)
  {
    fprintf(stderr, "inotify: inotify_add_watch() returned error: %s\n", strerror(r));
    return;
  }

  /* Open touch file for data read */
  touch_fd = open(touch_path, O_RDONLY);
  if(touch_fd < 0)
  {
    fprintf(stderr, "inotify: open() touch file returned error: %s\n", strerror(touch_fd));
    return;
  }

  char *p;
  ssize_t pending_length;
  struct inotify_event *event;
  char buf[INOTIFY_FD_BUFFER_LENGTH] __attribute__ ((aligned(8)));
  while (1)
  {
    /* Wait for new events */
    pending_length = read(inotify_fd, buf, INOTIFY_FD_BUFFER_LENGTH);
    if (pending_length <= 0)
    {
      continue;
    }

    /* Process buffer of new events */
    for (p = buf; p < buf + pending_length; )
    {
      event = (struct inotify_event *) p;

      /* Read data from touch file, and pass touch event to supplied callback */
      touch_readEvents(touch_fd, touch_callback);

      /* Iterate onwards */
      p += sizeof(struct inotify_event) + event->len;
    }
  }

  close(touch_fd);
  close(inotify_fd);
}

extern int64_t center_frequency;
extern int64_t span_frequency;

extern int64_t selected_center_frequency;
extern int64_t selected_span_frequency;

static bool main_drag_ongoing = false;
static int main_drag_last_pos_x = 0;

static bool if_drag_ongoing = false;
static int if_drag_last_pos_x = 0;

/* Hardcoded, TODO: Derive from graphics.h */
#define IF_WF_AX    544
#define IF_WF_AW    256
#define IF_WF_AY    148
#define IF_WF_AH    120

#define MAIN_WF_AX  0
#define MAIN_WF_AW  512
#define MAIN_WF_AY  180
#define MAIN_WF_AH  300

#define MAIN_WF_AX  0
#define MAIN_WF_AW  512
#define MAIN_WF_AY  180
#define MAIN_WF_AH  300

#define PTT_BTN_AX  645
#define PTT_BTN_AW  150
#define PTT_BTN_AY  375
#define PTT_BTN_AH  100

#define areaTouched(ax, aw, ay, ah)     ((touch_x > ax) && (touch_x < (ax + aw)) && (touch_y > ay) && (touch_y < (ay + ah)))
#define xTouched(ax, aw)                ((touch_x > ax) && (touch_x < (ax + aw)))

static void touch_process(int touch_type, int touch_x, int touch_y)
{
    if(touch_type == TOUCH_EVENT_START)
    {
        /* Main Waterfall tuning drag */
        if(!main_drag_ongoing
        && areaTouched(MAIN_WF_AX, MAIN_WF_AW, MAIN_WF_AY, MAIN_WF_AH))
        {
            main_drag_ongoing = true;
            selected_center_frequency = (center_frequency - (span_frequency / 2)) + ((((touch_x - MAIN_WF_AX) * span_frequency) / MAIN_WF_AW));
            graphics_frequency_newdata();
            main_drag_last_pos_x = touch_x;
        }

        /* IF Waterfall tuning drag */
        if(!if_drag_ongoing
        && areaTouched(IF_WF_AX, IF_WF_AW, IF_WF_AY, IF_WF_AH))
        {
            if_drag_ongoing = true;
            if_drag_last_pos_x = touch_x;
        }

        /* PTT Button */
        if(areaTouched(PTT_BTN_AX, PTT_BTN_AW, PTT_BTN_AY, PTT_BTN_AH))
        {
            ptt_pressed = true;
            ptt_button_generate();
            ptt_button_render();
        }
    }

    if(touch_type == TOUCH_EVENT_MOVE)
    {
        if(main_drag_ongoing
        && xTouched(MAIN_WF_AX, MAIN_WF_AW))
        {
            //printf(" - Freq += %lld.\n", (main_drag_last_pos_x - touch_x) * (span_frequency / MAIN_WF_AW));
            selected_center_frequency += (touch_x - main_drag_last_pos_x) * (span_frequency / MAIN_WF_AW);
            graphics_frequency_newdata();
            main_drag_last_pos_x = touch_x;
        }

        if(if_drag_ongoing
        && xTouched(IF_WF_AX, IF_WF_AW))
        {
            //printf(" - Freq += %lld.\n", (if_drag_last_pos_x - touch_x) * (selected_span_frequency / IF_WF_AW));
            selected_center_frequency += (if_drag_last_pos_x - touch_x) * (selected_span_frequency / IF_WF_AW);
            graphics_frequency_newdata();
            if_drag_last_pos_x = touch_x;
        }
    }

    if(touch_type == TOUCH_EVENT_END)
    {
        main_drag_ongoing = false;
        if_drag_ongoing = false;
        ptt_pressed = false;
    }
}

void *touch_thread(void *arg)
{
  bool *app_exit = (bool *)arg;
  (void)app_exit;

  char *touchscreen_path;

  if(!touch_detecthw(&touchscreen_path))
  {
    fprintf(stderr, "Error initialising touch!\n");
    return NULL;
  }
  
  touch_run(touchscreen_path, &touch_process);

  return NULL;
}