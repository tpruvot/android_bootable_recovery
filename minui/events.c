/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/poll.h>
#include <limits.h>

#include <linux/input.h>

#include "../common.h"

#include "minui.h"

#define MAX_DEVICES 16
#define MAX_MISC_FDS 16

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

#define test_bit(bit, array) \
    ((array)[(bit)/BITS_PER_LONG] & (1 << ((bit) % BITS_PER_LONG)))

#define VIBRATOR_TIMEOUT_FILE	"/sys/class/timed_output/vibrator/enable"
#define VIBRATOR_TIME_MS	50

#define PRESS_THRESHHOLD    10

#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TOUCH_MAJOR 0x30
#define SYN_MT_REPORT 2

#ifndef KEYPAD_USE_EV_ABS
#define KEYPAD_USE_EV_ABS 1
#endif

struct virtualkey {
    int scancode;
    int centerx, centery;
    int width, height;
};

struct position {
    int x, y;
    int pressed;
    struct input_absinfo xi, yi;
};

struct ev {
    struct pollfd *fd;

    struct virtualkey *vks;
    int vk_count;

    struct position p, mt_p;
    int sent, mt_idx;
};

struct fd_info {
    ev_callback cb;
    void *data;
};

static struct pollfd ev_fds[MAX_DEVICES + MAX_MISC_FDS];
static struct ev evs[MAX_DEVICES + MAX_MISC_FDS];

static struct fd_info ev_fdinfo[MAX_DEVICES + MAX_MISC_FDS];

static unsigned ev_count = 0;
static unsigned ev_dev_count = 0;
static unsigned ev_misc_count = 0;

static inline int ABS(int x) {
    return x<0?-x:x;
}

int vibrate(int timeout_ms)
{
    char str[20];
    int fd;
    int ret;

    fd = open(VIBRATOR_TIMEOUT_FILE, O_WRONLY);
    if (fd < 0)
        return -1;

    ret = snprintf(str, sizeof(str), "%d", timeout_ms);
    ret = write(fd, str, ret);
    close(fd);

    if (ret < 0)
       return -1;

    return 0;
}

/* Returns empty tokens */
static char *vk_strtok_r(char *str, const char *delim, char **save_str)
{
    if(!str) {
        if(!*save_str) return NULL;
        str = (*save_str) + 1;
    }
    *save_str = strpbrk(str, delim);
    if(*save_str) **save_str = '\0';
    return str;
}

// to reduce stack frame size
static char vks[2048];

static int vk_init(struct ev *e)
{
    char vk_path[PATH_MAX] = "/sys/board_properties/virtualkeys.";
    char *ts;
    ssize_t len;
    int vk_fd;
    int i;

    e->vk_count = 0;

    len = strlen(vk_path);
    len = ioctl(e->fd->fd, EVIOCGNAME(sizeof(vk_path) - len), vk_path + len);
    if (len <= 0)
        return -1;

    vk_fd = open(vk_path, O_RDONLY);
    if (vk_fd < 0)
        return -1;

    len = read(vk_fd, vks, sizeof(vks)-1);
    close(vk_fd);
    if (len <= 0)
        return -1;

    vks[len] = '\0';

    /* Parse a line like:
        keytype:keycode:centerx:centery:width:height:keytype2:keycode2:centerx2:...
    */
    for (ts = vks, e->vk_count = 1; *ts; ++ts) {
        if (*ts == ':')
            ++e->vk_count;
    }

    if (e->vk_count % 6) {
        LOGW("minui: %s is %d %% 6\n", vk_path, e->vk_count % 6);
    }
    e->vk_count /= 6;
    if (e->vk_count <= 0)
        return -1;

    e->sent = 0;
    e->mt_idx = 0;

    ioctl(e->fd->fd, EVIOCGABS(ABS_X), &e->p.xi);
    ioctl(e->fd->fd, EVIOCGABS(ABS_Y), &e->p.yi);
    e->p.pressed = 0;

    ioctl(e->fd->fd, EVIOCGABS(ABS_MT_POSITION_X), &e->mt_p.xi);
    ioctl(e->fd->fd, EVIOCGABS(ABS_MT_POSITION_Y), &e->mt_p.yi);
    e->mt_p.pressed = 0;

    e->vks = malloc(sizeof(*e->vks) * e->vk_count);

    LOGI("%s: found %d scancodes\n", vk_path, e->vk_count);

    for (i = 0; i < e->vk_count; ++i) {
        char *token[6];
        int j;

        for (j = 0; j < 6; ++j) {
            token[j] = vk_strtok_r((i||j)?NULL:vks, ":", &ts);
        }

        if (strcmp(token[0], "0x01") != 0) {
            /* Java does string compare, so we do too. */
            LOGW("minui: %s: ignoring unknown virtual key type %s\n", vk_path, token[0]);
            continue;
        }

        e->vks[i].scancode = strtol(token[1], NULL, 0);
        e->vks[i].centerx = strtol(token[2], NULL, 0);
        e->vks[i].centery = strtol(token[3], NULL, 0);
        e->vks[i].width = strtol(token[4], NULL, 0);
        e->vks[i].height = strtol(token[5], NULL, 0);
    }

    return 0;
}

int ev_init(void)
{
    DIR *dir;
    struct dirent *de;
    int fd;

    dir = opendir("/dev/input");
    if(dir != 0) {
        while((de = readdir(dir))) {
            unsigned long ev_bits[BITS_TO_LONGS(EV_MAX)];

//            fprintf(stderr,"/dev/input/%s\n", de->d_name);
            if(strncmp(de->d_name,"event",5)) continue;
            fd = openat(dirfd(dir), de->d_name, O_RDONLY);
            if(fd < 0) continue;

            /* read the evbits of the input device */
            if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
                close(fd);
                continue;
            }

            /* TODO: add ability to specify event masks. For now, just assume
             * that only EV_KEY and EV_REL event types are ever needed. */
            if (!test_bit(EV_KEY, ev_bits) && !test_bit(EV_REL, ev_bits)) {
                #if KEYPAD_USE_EV_ABS
                if (!test_bit(EV_ABS, ev_bits))
                #endif
                {
                    close(fd);
                    continue;
                }
            }

            ev_fds[ev_count].fd = fd;
            ev_fds[ev_count].events = POLLIN;
            evs[ev_count].fd = &ev_fds[ev_count];

            /* Load virtualkeys if there are any */
            vk_init(&evs[ev_count]);

            ev_count++;
            ev_dev_count++;
            if(ev_dev_count == MAX_DEVICES) break;
        }
    }

    return 0;
}

int ev_add_fd(int fd, ev_callback cb, void *data)
{
    if (ev_misc_count == MAX_MISC_FDS || cb == NULL)
        return -1;
    ev_fds[ev_count].fd = fd;
    ev_fds[ev_count].events = POLLIN;
    ev_fdinfo[ev_count].cb = cb;
    ev_fdinfo[ev_count].data = data;
    ev_count++;
    ev_misc_count++;
    return 0;
}

void ev_exit(void)
{
    while (ev_count-- > 0) {
	if (evs[ev_count].vk_count) {
		free(evs[ev_count].vks);
		evs[ev_count].vk_count = 0;
	}
        close(ev_fds[ev_count].fd);
    }
    ev_misc_count = 0;
    ev_dev_count = 0;
}

int ev_wait(int timeout)
{
    int r = poll(ev_fds, ev_count, timeout);
    if (r <= 0)
        return -1;
    return 0;
}

void ev_dispatch(void)
{
    unsigned n;
    int ret;
    for (n = 0; n < ev_count; n++) {
        ev_callback cb = ev_fdinfo[n].cb;
        if (cb && (ev_fds[n].revents & ev_fds[n].events))
            cb(ev_fds[n].fd, ev_fds[n].revents, ev_fdinfo[n].data);
    }
}

int ev_get_input(int fd, short revents, struct input_event *ev)
{
    int r;
    if (revents & POLLIN) {
        r = read(fd, ev, sizeof(*ev));
        if (r == sizeof(*ev))
            return 0;
    }
    return -1;
}

int ev_sync_key_state(ev_set_key_callback set_key_cb, void *data)
{
    unsigned long key_bits[BITS_TO_LONGS(KEY_MAX)];
    unsigned long ev_bits[BITS_TO_LONGS(EV_MAX)];
    unsigned i;
    int ret;

    for (i = 0; i < ev_dev_count; i++) {
        int code;

        memset(key_bits, 0, sizeof(key_bits));
        memset(ev_bits, 0, sizeof(ev_bits));

        ret = ioctl(ev_fds[i].fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
        if (ret < 0 || !test_bit(EV_KEY, ev_bits))
            continue;

        ret = ioctl(ev_fds[i].fd, EVIOCGKEY(sizeof(key_bits)), key_bits);
        if (ret < 0)
            continue;

        for (code = 0; code <= KEY_MAX; code++) {
            if (test_bit(code, key_bits))
                set_key_cb(code, 1, data);
        }
    }

    return 0;
}

static int vk_inside_display(__s32 value, struct input_absinfo *info, int screen_size)
{
    int screen_pos;

    if (info->minimum == info->maximum)
        return 0;

    screen_pos = (value - info->minimum) * (screen_size - 1) / (info->maximum - info->minimum);
    return (screen_pos >= 0 && screen_pos < screen_size);
}

static int vk_tp_to_screen(struct position *p, int *x, int *y)
{
    if (p->xi.minimum == p->xi.maximum || p->yi.minimum == p->yi.maximum)
        return 0;

    *x = (p->x - p->xi.minimum) * (gr_fb_width() - 1) / (p->xi.maximum - p->xi.minimum);
    *y = (p->y - p->yi.minimum) * (gr_fb_height() - 1) / (p->yi.maximum - p->yi.minimum);

    if (*x >= 0 && *x < gr_fb_width() &&
           *y >= 0 && *y < gr_fb_height()) {
        return 0;
    }

    return 1;
}

/* Translate a virtual key in to a real key event, if needed */
/* Returns non-zero when the event should be consumed */
static int vk_modify(struct ev *e, struct input_event *ev)
{
    int i;
    int x, y;

    if (ev->type == EV_KEY) {
        if (ev->code == BTN_TOUCH)
            e->p.pressed = ev->value;
        return 0;
    }

    if (ev->type == EV_ABS) {
        switch (ev->code) {
        case ABS_X:
            e->p.x = ev->value;
            return !vk_inside_display(e->p.x, &e->p.xi, gr_fb_width());
        case ABS_Y:
            e->p.y = ev->value;
            return !vk_inside_display(e->p.y, &e->p.yi, gr_fb_height());
        case ABS_MT_POSITION_X:
            if (e->mt_idx) return 1;
            e->mt_p.x = ev->value;
            return !vk_inside_display(e->mt_p.x, &e->mt_p.xi, gr_fb_width());
        case ABS_MT_POSITION_Y:
            if (e->mt_idx) return 1;
            e->mt_p.y = ev->value;
            return !vk_inside_display(e->mt_p.y, &e->mt_p.yi, gr_fb_height());
        case ABS_MT_TOUCH_MAJOR:
            if (e->mt_idx) return 1;
            if (e->sent)
                e->mt_p.pressed = (ev->value > 0);
            else
                e->mt_p.pressed = (ev->value > PRESS_THRESHHOLD);
            return 0;
        }

        return 0;
    }

    if (ev->type != EV_SYN)
        return 0;

    if (ev->code == SYN_MT_REPORT) {
        /* Ignore the rest of the points */
        ++e->mt_idx;
        return 1;
    }
    if (ev->code != SYN_REPORT)
        return 0;

    /* Report complete */

    e->mt_idx = 0;

    if (!e->p.pressed && !e->mt_p.pressed) {
        /* No touch */
        e->sent = 0;
        return 0;
    }

    if (!(e->p.pressed && vk_tp_to_screen(&e->p, &x, &y)) &&
            !(e->mt_p.pressed && vk_tp_to_screen(&e->mt_p, &x, &y))) {
        /* No touch inside vk area */
        return 0;
    }

    if (e->sent) {
        /* We've already sent a fake key for this touch */
        return 1;
    }

    /* The screen is being touched on the vk area */
    e->sent = 1;

    for (i = 0; i < e->vk_count; ++i) {
        int xd = ABS(e->vks[i].centerx - x);
        int yd = ABS(e->vks[i].centery - y);
        if (xd < e->vks[i].width/2 && yd < e->vks[i].height/2) {
            /* Fake a key event */
            ev->type = EV_KEY;
            ev->code = e->vks[i].scancode;
            ev->value = 1;

            vibrate(VIBRATOR_TIME_MS);
            return 0;
        }
    }

    return 1;
}

int ev_get(struct input_event *ev, unsigned dont_wait)
{
    int r;
    unsigned n;

    do {
        r = poll(ev_fds, ev_count, dont_wait ? 0 : -1);

        if(r > 0) {
            for(n = 0; n < ev_count; n++) {
                if(ev_fds[n].revents & POLLIN) {
                    r = read(ev_fds[n].fd, ev, sizeof(*ev));
                    if(r == sizeof(*ev)) {
                        if (!vk_modify(&evs[n], ev))
                            return 0;
                    }
                }
            }
        }
    } while(dont_wait == 0);

    return -1;
}

