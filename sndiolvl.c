/*
 * Copyright (c) 2016 Percy Piper <piper.percy@googlemail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/ioctl.h>

#include <err.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gst/gst.h>

#define MAXCHANS 8
#define AMBER 0.67   /* -3.5 dBFS */
#define RED 0.79     /* -2.0 dBFS */
#define CLIP 0.00    /* dBFS */
#define HOLD 24      /* peak and clip indicators */
#define OVER 3       /* consecutive samples at CLIP */
#define STATUSCHARS 42 /* width of status region */

int TERMWIDTH = 0;
int refresh = 1;
int hold[MAXCHANS] = {HOLD};
int clip[MAXCHANS] = {0};
double peakhold[MAXCHANS] = {0};
int over[MAXCHANS] = {0};
ulong overs[MAXCHANS] = {0};

static volatile sig_atomic_t done = 0;

static void update_meter(double, double, double, int);
static gboolean gst_msg_handler(GstBus *, GstMessage *, gpointer);
static void get_term_width();
static void catch_sigint(int);

static void
update_meter(double peak, double rms, double peakhold, int clip)
{
    int width = TERMWIDTH - STATUSCHARS;
    int wpeak = (int) width * peak;
    int wrms = (int) width * rms;
    int amber = (int) width * AMBER;
    int red = (int) width * RED;
    int hold = (int) width * peakhold -2;

    char bar[width];
    memset (bar, '=',  (sizeof(bar)));

    /* peak */
    if (peak >= RED) {
        printf ("\033[34m%.*s\033[33m%.*s\033[31m%-*.*s\033[0m]\r", 
            amber, bar,
            (width - amber) - (width - red), bar,
            width - red, wpeak - red, bar);
    } else if (peak >= AMBER) {
        printf ("\033[34m%.*s\033[33m%-*.*s\033[0m]\r",
            amber, bar,
            width - amber, wpeak - amber, bar);
    } else
        printf ("\033[34m%-*.*s\033[0m]\r",
            width, wpeak, bar);

    /* rms & peakhold */
    if (peakhold >= RED) {
        printf ("[\033[32m%.*s\033[31m\033[%dC+\033[0m\r",
            wrms, bar, hold - wrms);
    } else if (peakhold >= AMBER) {
        printf ("[\033[32m%.*s\033[33m\033[%dC+\033[0m\r",
            wrms, bar, hold - wrms);
    } else
        printf ("[\033[32m%.*s\033[34m\033[%dC+\033[0m\r",
            wrms, bar, hold - wrms);

    /* clip */
    if (clip > 0)
        printf ("\033[31m\033[%dC!\033[0m\r", width -1);
}

static gboolean
gst_msg_handler(GstBus * bus, GstMessage * message, gpointer p)
{
    int channels, i;
    double rms_dB, peak_dB, rms, peak;
    const GValue *array_value, *value;
    GValueArray *rms_array, *peak_array;
    GMainLoop *loop = p;

    if (done) {
        printf ("\033[1K\033[J\n");
        g_main_loop_quit(loop);
    }

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
    {
        printf ("\033[1K\033[J\n");
        printf ("End of stream\n");
        g_main_loop_quit(loop);
    }
    case GST_MESSAGE_WARNING:
    case GST_MESSAGE_ERROR:
    {
        gchar *debug = NULL;
        GError *err = NULL;

        printf ("\033[1K\033[J\n");
        gst_message_parse_error(message, &err, &debug);
        printf ("\n\nError %s: %s\n",  GST_OBJECT_NAME(message->src), \
            err->message);
        g_error_free(err);

        printf ("Debug: %s\n", debug);
        g_free(debug);

        g_main_loop_quit(loop);
        exit (FALSE);
    }
    case GST_MESSAGE_ELEMENT:
    {
        const GstStructure *s = gst_message_get_structure(message);
        const gchar *name = gst_structure_get_name(s);

        if (strcmp(name, "level") == 0) {
            array_value = gst_structure_get_value(s, "rms");
            rms_array = (GValueArray *) g_value_get_boxed(array_value);

            array_value = gst_structure_get_value(s, "peak");
            peak_array = (GValueArray *) g_value_get_boxed(array_value);

            channels = rms_array->n_values;
            if (channels > MAXCHANS)
                channels = MAXCHANS;

            for (i = 0; i < channels; ++i) {

				value = peak_array->values + i;
                peak_dB = g_value_get_double(value);    /* AES17-1998 */

                value = rms_array->values + i;
                rms_dB = g_value_get_double(value);

                peak = pow(10, peak_dB / 20);
                rms = pow(10, rms_dB / 20);

                if (peak_dB >= CLIP) {
                    refresh = 1;
                    over[i]++;
                    clip[i] = HOLD * 2;
                    if (over[i] >= OVER) {
                        overs[i]++;
                    }
                } else {
                    over[i] = 0;
                }

                if (peakhold[i] < peak) {
                    peakhold[i] = peak;
                    refresh = 1;
                    hold[i] = HOLD;
                }

                update_meter(peak, rms, peakhold[i], clip[i]);

                if (refresh) {
                    printf ("\033[%dC Peak %7.2fdB RMS %7.2fdB Over %-5lu\r",
                        TERMWIDTH -(STATUSCHARS -1), peak_dB, rms_dB, overs[i]);
                }

                refresh = 0;

                if (hold[i] >= 1) {
                    hold[i] -= 1;
                } else {
                    hold[i] = 0;
                    if (peakhold[i] >= 0.02) {
                        peakhold[i] -= 0.02;
                    } else {
                        peakhold[i] = 0;
                    }
                }

                if (clip[i] >= 1)
                    clip[i] -= 1;

                if (i <= (channels -2))
                    printf ("\n");
            }

            if (channels > 1)
                printf ("\033[%dA", channels -1);
        }
    }
    default:
        break;
    }
    return TRUE;
}

static void
get_term_width()
{
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    TERMWIDTH = w.ws_col;
    refresh = 1;
    printf ("\033[J");
}

static void
catch_sigint(int signo)
{
    done = 1;
}

int
main (int argc, char *argv[])
{
    if (pledge("stdio rpath wpath cpath exec prot_exec proc unix audio tty",
        NULL) == -1)
        err(1, "pledge");

    setvbuf(stdout, NULL, _IONBF, 0);

    GstStateChangeReturn ret;
    GstElement *sndiosrc, *audioconvert, *level, *fakesink, *pipeline;
    GstCaps *caps;
    GstBus *bus;
    guint watch;
    GMainLoop *loop;

    signal(SIGWINCH, get_term_width);
    signal(SIGINT, catch_sigint);

    get_term_width();
    gst_init(&argc, &argv);

    caps = gst_caps_from_string("audio/x-raw");
    loop = g_main_loop_new(NULL, FALSE);
    pipeline = gst_pipeline_new(NULL);
    g_assert(pipeline);

    bus = gst_pipeline_get_bus(GST_PIPELINE (pipeline));
    watch = gst_bus_add_watch(bus, gst_msg_handler, loop);
    gst_object_unref(bus);

    sndiosrc = gst_element_factory_make("sndiosrc", NULL);
    g_assert(sndiosrc);

    audioconvert = gst_element_factory_make("audioconvert", NULL);
    g_assert(audioconvert);

    level = gst_element_factory_make("level", NULL);
    g_assert(level);

    fakesink = gst_element_factory_make("fakesink", NULL);
    g_assert(fakesink);

    gst_bin_add_many(GST_BIN (pipeline), sndiosrc, audioconvert, level,
        fakesink, NULL);
    if (!gst_element_link_filtered(sndiosrc, audioconvert, caps))
        g_error("Error linking sndiosrc and audioconvert");
    if (!gst_element_link(audioconvert, level))
        g_error("Error linking audioconvert and level");
    if (!gst_element_link(level, fakesink))
        g_error("Error linking level and fakesink");

    g_object_set(G_OBJECT (level), "post-messages", TRUE, NULL);
//    g_object_set(G_OBJECT (level), "interval", 75000000, NULL);
    g_object_set(G_OBJECT (fakesink), "sync", TRUE, NULL);

    ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        printf ("Failed to start pipeline\n");
        return FALSE;
    }

    while (!done)
        g_main_loop_run(loop);

    if (g_main_loop_is_running(loop))
        g_main_loop_quit(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_source_remove(watch);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);

    printf ("\033[1K\033[J\n");

    return TRUE;
}
