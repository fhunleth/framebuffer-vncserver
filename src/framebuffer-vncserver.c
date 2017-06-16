/*
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * This project is an adaptation of the original fbvncserver for the iPAQ
 * and Zaurus.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include <err.h>

/* libvncserver */
#include "rfb/rfb.h"
#include "rfb/keysym.h"

//#define DEBUG

/*****************************************************************************/

static char FB_DEVICE[256] = "/dev/fb0";
static struct fb_var_screeninfo scrinfo;
static struct fb_fix_screeninfo fixscrinfo;
static int fbfd = -1;
static const uint32_t *fbmmap = MAP_FAILED;
static uint32_t *vncbuf = NULL;
static int uinputfd = -1;

static int VNC_PORT = 5900;
static rfbScreenInfoPtr vncscr;

/*****************************************************************************/
static void init_uinput()
{
    uinputfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinputfd < 0)
        errx(EXIT_FAILURE, "uinput kernel module required");

    if (ioctl(uinputfd, UI_SET_EVBIT, EV_KEY) < 0)
        err(EXIT_FAILURE, "ioctl(UI_SET_EVBIT)");
    if (ioctl(uinputfd, UI_SET_KEYBIT, BTN_TOUCH) < 0)
        err(EXIT_FAILURE, "ioctl(UI_SET_KEYBIT)");
    if (ioctl(uinputfd, UI_SET_EVBIT, EV_ABS) < 0)
        err(EXIT_FAILURE, "ioctl(UI_SET_EVBIT)");
    if (ioctl(uinputfd, UI_SET_ABSBIT, ABS_X) < 0)
        err(EXIT_FAILURE, "ioctl(UI_SET_ABSBIT)");
    if (ioctl(uinputfd, UI_SET_ABSBIT, ABS_Y) < 0)
        err(EXIT_FAILURE, "ioctl(UI_SET_ABSBIT)");

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "framebuffer-vncserver");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x1;
    uidev.id.product = 0x1;
    uidev.id.version = 1;

    uidev.absmin[ABS_X] = 0;
    uidev.absmin[ABS_Y] = 0;
    uidev.absmax[ABS_X] = scrinfo.xres - 1;
    uidev.absmax[ABS_Y] = scrinfo.yres - 1;

    if (write(uinputfd, &uidev, sizeof(uidev)) < 0)
        err(EXIT_FAILURE, "write(uidev)");

    if (ioctl(uinputfd, UI_DEV_CREATE) < 0)
        err(EXIT_FAILURE, "ioctl(UI_DEV_CREATE)");

    fprintf(stderr, "Initialized uinput\n");
}

static void init_fb(void)
{
    if ((fbfd = open(FB_DEVICE, O_RDONLY)) == -1)
        err(EXIT_FAILURE, "open %s", FB_DEVICE);

    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &scrinfo) != 0)
        err(EXIT_FAILURE, "ioctl(FBIOGET_VSCREENINFO)");

    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &fixscrinfo) != 0)
        err(EXIT_FAILURE, "ioctl(FBIOGET_FSCREENINFO)");

#ifdef DEBUG
    fprintf(stderr, "xres=%d, yres=%d, xresv=%d, yresv=%d, xoffs=%d, yoffs=%d, bpp=%d\n",
            (int)scrinfo.xres, (int)scrinfo.yres,
            (int)scrinfo.xres_virtual, (int)scrinfo.yres_virtual,
            (int)scrinfo.xoffset, (int)scrinfo.yoffset,
            (int)scrinfo.bits_per_pixel);

    fprintf(stderr, "line_length=%d\n",
            (int)fixscrinfo.line_length);
#endif

    fbmmap = mmap(NULL, fixscrinfo.line_length * scrinfo.yres, PROT_READ, MAP_SHARED, fbfd, 0);
    if (fbmmap == MAP_FAILED)
        err(EXIT_FAILURE, "mmap");

    if (scrinfo.xres & 0x3) {
        /* libvncserver complains when the xres is not divisible by 4. If
         * the framebuffer has space (it might), pad it out.
         */
        int desired_xres = (scrinfo.xres + 3) & ~0x3;
        if (desired_xres * scrinfo.bits_per_pixel / 8 < fixscrinfo.line_length)
            scrinfo.xres = desired_xres;
    }
}

static void cleanup_fb(void)
{
    if (fbfd != -1) {
        close(fbfd);
        fbfd = -1;
    }
}

static void ptrAddEvent(int buttonMask, int x, int y, rfbClientPtr cl)
{
#ifdef DEBUG
    fprintf(stderr, "Pointer: %d, %d, %d\n", x, y, buttonMask);
#endif
    if (buttonMask) {
        struct input_event ev[6];
        memset(&ev, 0, sizeof(ev));

        ev[0].type = EV_KEY;
        ev[0].code = BTN_TOUCH;
        ev[0].value = 1;
        ev[1].type = EV_ABS;
        ev[1].code = ABS_X;
        ev[1].value = x;
        ev[2].type = EV_ABS;
        ev[2].code = ABS_Y;
        ev[2].value = y;
        ev[3].type = EV_SYN;

        ev[4].type = EV_KEY;
        ev[4].code = BTN_TOUCH;
        ev[4].value = 0;
        ev[5].type = EV_SYN;

        if (write(uinputfd, &ev, sizeof(ev)) < 0)
            warn("error sending event");

    }

    rfbDefaultPtrAddEvent(buttonMask, x, y, cl);
}

/*****************************************************************************/

static void init_fb_server(int argc, char **argv)
{
#ifdef DEBUG
    fprintf(stderr, "Initializing server...\n");
#endif

    /* Allocate the VNC server buffer to be managed (not manipulated) by
     * libvncserver. */
    vncbuf = calloc(scrinfo.xres * scrinfo.yres, scrinfo.bits_per_pixel / 8);
    if (vncbuf == NULL)
        err(EXIT_FAILURE, "calloc");

    vncscr = rfbGetScreen(&argc, argv, scrinfo.xres, scrinfo.yres, 8, 3, (scrinfo.bits_per_pixel / 8));
    if (vncscr == NULL)
        errx(EXIT_FAILURE, "rfbGetScreen");

    /* We're running in ARGB so update the shifts so that we don't have to convert */
    vncscr->serverFormat.redShift = 16;
    vncscr->serverFormat.greenShift = 8;
    vncscr->serverFormat.blueShift = 0;
    vncscr->serverFormat.bigEndian = 0;

    vncscr->desktopName = "framebuffer";
    vncscr->frameBuffer = (char *)vncbuf;
    vncscr->alwaysShared = TRUE;
    vncscr->httpDir = NULL;
    vncscr->port = VNC_PORT;

    //	vncscr->kbdAddEvent = keyevent;
    vncscr->ptrAddEvent = ptrAddEvent;

    rfbInitServer(vncscr);

    /* Mark as dirty since we haven't sent any updates at all yet. */
    rfbMarkRectAsModified(vncscr, 0, 0, scrinfo.xres, scrinfo.yres);
}

/*****************************************************************************/

struct rect {
    int x1;
    int y1;
    int x2;
    int y2;
};

static void union_rect(struct rect *r, const struct rect *other)
{
    if (other->x1 > other->x2)
        return;

    if (r->x1 > other->x1)
        r->x1 = other->x1;
    if (r->y1 > other->y1)
        r->y1 = other->y1;
    if (r->x2 < other->x2)
        r->x2 = other->x2;
    if (r->y2 < other->y2)
        r->y2 = other->y2;
}

static inline void union_point(struct rect *r, int x, int y)
{
    if (x < r->x1)
        r->x1 = x;
    if (y < r->y1)
        r->y1 = y;

    if (x > r->x2)
        r->x2 = x;
    if (y > r->y2)
        r->y2 = y;
}

static void print_rect(const char *caption, struct rect *r)
{
    if (r->x1 <= r->x2)
        fprintf(stderr, "%s: %d,%d->%d,%d\n", caption, r->x1, r->y1, r->x2, r->y2);
    else
        fprintf(stderr, "%s: empty\n", caption);
}

static struct rect find_diff_rect(int left, int top, int right, int bottom, int offset, int skip)
{
    int x, y;
    struct rect result;
    result.x1 = INT_MAX;
    result.y1 = INT_MAX;
    result.x2 = -1;
    result.y2 = -1;

    const uint32_t *f = fbmmap + (top + offset) * (fixscrinfo.line_length / sizeof(uint32_t));
    uint32_t *r = vncbuf + (top + offset) * scrinfo.xres;
    for (y = top + offset; y <= bottom; y += skip) {
        for (x = left + offset; x <= right; x += skip) {
            if (f[x] != r[x])
                union_point(&result, x, y);
        }
        f += skip * fixscrinfo.line_length / sizeof(uint32_t);
        r += skip * scrinfo.xres;
    }

    if (result.x1 <= result.x2 && skip > 1) {
        if (skip > 1) {
            /* If skipping, check the regions that we skipped. */
            int outer_left = result.x1 - offset;
            int outer_top = result.y1 - offset;
            int outer_right = result.x2 + skip - offset - 1;
            int outer_bottom = result.y2 + skip - offset - 1;
            if (outer_right > right)
                outer_right = right;
            if (outer_bottom > bottom)
                outer_bottom = bottom;

            struct rect r = find_diff_rect(outer_left, outer_top, outer_right, result.y1 - 1, 0, 1);
            union_rect(&result, &r);
            r = find_diff_rect(outer_left, result.y2 + 1, outer_right, outer_bottom, 0, 1);
            union_rect(&result, &r);
            r = find_diff_rect(outer_left, result.y1, result.x1 - 1, result.y2, 0, 1);
            union_rect(&result, &r);
            r = find_diff_rect(result.x2 + 1, result.y1, outer_right, result.y2, 0, 1);
            union_rect(&result, &r);
        }
    }
    return result;
}

static void update_rect(int left, int top, int right, int bottom, int offset, int skip)
{
    int x, y;
    struct rect delta = find_diff_rect(left, top, right, bottom, offset, skip);

    if (delta.x1 <= delta.x2) {
#ifdef DEBUG
        fprintf(stderr, "Dirty page: %dx%d+%d+%d...\n",
                (delta.x2+1) - delta.x1, (delta.y2+1) - delta.y1,
                delta.x1, delta.y1);
#endif
        const uint32_t *f = fbmmap + delta.y1 * (fixscrinfo.line_length / sizeof(uint32_t));
        uint32_t *r = vncbuf + delta.y1 * scrinfo.xres;
        for (y = delta.y1; y <= delta.y2; y++) {
            for (x = delta.x1; x <= delta.x2; x++)
                r[x] = f[x];

            f += fixscrinfo.line_length / sizeof(uint32_t);
            r += scrinfo.xres;
        }

        /* rfbMarkRectAsModified wants non-inclusive x2, y2 */
        rfbMarkRectAsModified(vncscr, delta.x1, delta.y1, delta.x2 + 1, delta.y2 + 1);
    }
}

static void update_screen()
{
    const int skip = 16;
    static int offset = 0;

    update_rect(0, 0, scrinfo.xres - 1, scrinfo.yres - 1, offset, skip);
    offset++;
    if (offset == skip)
        offset = 0;
}

/*****************************************************************************/

void print_usage(char **argv)
{
    fprintf(stderr, "%s [-f device] [-p port] [-h]\n"
                    "-p port: VNC port, default is 5900\n"
                    "-f device: framebuffer device node, default is /dev/fb0\n"
                    "-h : print this help\n"
            , *argv);
}

static uint64_t getmicros()
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec * 1000000ULL + tv.tv_usec;
}

int main(int argc, char **argv)
{
    if(argc > 1)
    {
        int i=1;
        while(i < argc)
        {
            if(*argv[i] == '-')
            {
                switch(*(argv[i] + 1))
                {
                case 'h':
                    print_usage(argv);
                    exit(0);
                    break;
                case 'f':
                    i++;
                    strcpy(FB_DEVICE, argv[i]);
                    break;
                case 'p':
                    i++;
                    VNC_PORT = atoi(argv[i]);
                    break;
                }
            }
            i++;
        }
    }

    fprintf(stderr, "Initializing framebuffer device %s...\n", FB_DEVICE);
    init_fb();
    init_uinput();

    fprintf(stderr, "Initializing VNC server:\n");
    fprintf(stderr, "	width:  %d\n", (int)scrinfo.xres);
    fprintf(stderr, "	height: %d\n", (int)scrinfo.yres);
    fprintf(stderr, "	bpp:    %d\n", (int)scrinfo.bits_per_pixel);
    fprintf(stderr, "	port:   %d\n", (int)VNC_PORT);
    init_fb_server(argc, argv);

    /* Implement our own event loop to detect changes in the framebuffer. */
    uint64_t next_update = 0;
    while (1)
    {
        while (vncscr->clientHead == NULL)
            rfbProcessEvents(vncscr, 100000);

        rfbProcessEvents(vncscr, 100000);

        uint64_t now = getmicros();
        if (now > next_update) {
            update_screen();

#ifdef DEBUG
            uint64_t update_time = getmicros() - now;
            if (update_time > 10000)
                fprintf(stderr, "update took %d us\n", update_time);
#endif

            next_update = now + 100000; // Update 10 time/sec max
        }
    }

    fprintf(stderr, "Cleaning up...\n");
    cleanup_fb();
}
