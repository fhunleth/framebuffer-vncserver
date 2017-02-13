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

//#define DEBUG

/*****************************************************************************/

static char FB_DEVICE[256] = "/dev/fb0";
static struct fb_var_screeninfo scrinfo;
static struct fb_fix_screeninfo fixscrinfo;
static int fbfd = -1;
static unsigned short int *fbmmap = MAP_FAILED;
static unsigned short int *vncbuf;

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
    //	vncscr->ptrAddEvent = ptrevent;

    rfbInitServer(vncscr);

    /* Mark as dirty since we haven't sent any updates at all yet. */
    rfbMarkRectAsModified(vncscr, 0, 0, scrinfo.xres, scrinfo.yres);
}

/*****************************************************************************/

static void update_screen(void)
{
    const unsigned int *f;
    unsigned int *r;
    int x, y;

    int min_i, min_j, max_i, max_j;

    min_i = min_j = 9999;
    max_i = max_j = -1;

    f = (const unsigned int *)fbmmap;        /* -> framebuffer         */
    r = (unsigned int *)vncbuf;        /* -> remote framebuffer  */

    for (y = 0; y < scrinfo.yres; y++)
    {
        for (x = 0; x < scrinfo.xres; x++)
        {
            unsigned int pixel = f[x];
            pixel = ((pixel & 0xff0000) >> 16) | (pixel & 0x00ff00) | ((pixel & 0xff) << 16);

            if (pixel != r[x])
            {
                r[x] = pixel;

                if (x < min_i)
                    min_i = x;
                else
                {
                    if (x > max_i)
                        max_i = x;

                    if (y > max_j)
                        max_j = y;
                    else if (y < min_j)
                        min_j = y;
                }
            }
        }
        f += fixscrinfo.line_length / sizeof(unsigned int);
        r += scrinfo.xres;
    }

    if (min_i < 9999)
    {
        if (max_i < 0)
            max_i = min_i;

        if (max_j < 0)
            max_j = min_j;

#ifdef DEBUG
        fprintf(stderr, "Dirty page: %dx%d+%d+%d...\n",
                (max_i+2) - min_i, (max_j+1) - min_j,
                min_i, min_j);
#endif

        rfbMarkRectAsModified(vncscr, min_i, min_j,
                              max_i + 2, max_j + 1);

        rfbProcessEvents(vncscr, 10000);
    }
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
    while (1)
    {
        while (vncscr->clientHead == NULL)
            rfbProcessEvents(vncscr, 100000);

        rfbProcessEvents(vncscr, 100000);
        update_screen();
    }

    fprintf(stderr, "Cleaning up...\n");
    cleanup_fb();
}
