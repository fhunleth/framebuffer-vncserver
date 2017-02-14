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

#include <err.h>

/* libvncserver */
#include "rfb/rfb.h"
#include "rfb/keysym.h"

#define DEBUG

/*****************************************************************************/

static char FB_DEVICE[256] = "/dev/fb0";
static struct fb_var_screeninfo scrinfo;
static struct fb_fix_screeninfo fixscrinfo;
static int fbfd = -1;
static const uint32_t *fbmmap = MAP_FAILED;
static uint32_t *vncbuf = NULL;

static int VNC_PORT = 5900;
static rfbScreenInfoPtr vncscr;

/*****************************************************************************/

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
    fprintf(stderr, "Pointer: %d, %d, %d\n", x, y, buttonMask);
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

static void update_rect(int left, int top, int right, int bottom, int offset, int skip)
{
    int min_x = right + 1;
    int min_y = bottom + 1;
    int max_x = left - 1;
    int max_y = top - 1;

    const uint32_t *f = fbmmap + (top + offset) * (fixscrinfo.line_length / sizeof(uint32_t));
    uint32_t *r = vncbuf + (top + offset) * scrinfo.xres;

    for (int y = top + offset; y <= bottom; y += skip) {
        for (int x = left + offset; x <= right; x += skip) {
            uint32_t pixel = f[x];
            pixel = ((pixel & 0xff0000) >> 16) | (pixel & 0x00ff00) | ((pixel & 0xff) << 16);

            if (pixel != r[x]) {
                r[x] = pixel;

                if (x < min_x)
                    min_x = x;
                if (x > max_x)
                    max_x = x;

                if (y > max_y)
                    max_y = y;
                if (y < min_y)
                    min_y = y;
            }
        }
        f += skip * fixscrinfo.line_length / sizeof(uint32_t);
        r += skip * scrinfo.xres;
    }

    if (min_x <= max_x) {
#ifdef DEBUG
        fprintf(stderr, "Dirty page: %dx%d+%d+%d...\n",
                (max_x+1) - min_x, (max_y+1) - min_y,
                min_x, min_y);
#endif
        f = fbmmap + min_y * (fixscrinfo.line_length / sizeof(uint32_t));
        r = vncbuf + min_y * scrinfo.xres;
        for (int y = min_y; y <= max_y; y++) {
            for (int x = min_x; x <= max_x; x++) {
                uint32_t pixel = f[x];
                r[x] = ((pixel & 0xff0000) >> 16) | (pixel & 0x00ff00) | ((pixel & 0xff) << 16);
           }
        }
        rfbMarkRectAsModified(vncscr, min_x, min_y, max_x + 1, max_y + 1);

        if (skip > 1) {
            /* If skipping, check the regions that we skipped. */
            int outer_left = min_x - offset;
            int outer_top = min_y - offset;
            int outer_right = max_x + skip - offset - 1;
            int outer_bottom = max_y + skip - offset - 1;
            if (outer_right > right)
                outer_right = right;
            if (outer_bottom > bottom)
                outer_bottom = bottom;
            update_rect(outer_left, outer_top, outer_right, min_y - 1, 0, 1);
            update_rect(outer_left, min_y, min_x - 1, max_y, 0, 1);
            update_rect(max_x + 1, min_y, outer_right, max_y, 0, 1);
            update_rect(outer_left, min_y + 1, outer_right, outer_bottom, 0, 1);
        }
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

            uint64_t update_time = getmicros() - now;
            if (update_time > 10000)
                fprintf(stderr, "update took %d us\n", update_time);

            next_update = now + 100000; // Update 10 time/sec max
        }
    }

    fprintf(stderr, "Cleaning up...\n");
    cleanup_fb();
}
