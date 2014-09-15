/*
 * Copyright 2012 Andrew Eikum for CodeWeavers Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "spiceqxl_audio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>

#define BUFFER_PERIODS 10
#define PERIOD_MS 10
#define MAX_FIFOS 16

struct audio_data {
    int fifo_fds[MAX_FIFOS];
    ino_t inodes[MAX_FIFOS];
    uint32_t valid_bytes, write_offs;
    char *buffer, *spice_buffer;
    int period_frames;
    uint32_t spice_write_offs, spice_buffer_bytes;
    uint32_t frame_bytes, period_bytes, fed, buffer_bytes;
    struct timeval last_read_time;
};

static ssize_t
read_from_fifos (struct audio_data *data)
{
    size_t to_read_bytes = min(data->period_bytes, data->buffer_bytes - data->write_offs);
    int i;
    ssize_t max_readed = 0;
    int16_t *out_buf = (int16_t*)(data->buffer + data->write_offs), *buf;

    buf = malloc(to_read_bytes);
    if (!buf)
    {
        ErrorF("playback: malloc failed: %s\n", strerror(errno));
        return 0;
    }

    memset(out_buf, 0, to_read_bytes);

    for (i = 0; i < MAX_FIFOS; ++i)
    {
        unsigned int s;
        ssize_t readed;

        if (data->fifo_fds[i] < 0)
            continue;

        readed = read(data->fifo_fds[i], buf, to_read_bytes);
        if (readed < 0)
        {
            if (errno != EAGAIN && errno != EINTR)
                ErrorF("playback: read from FIFO %d failed: %s\n", data->fifo_fds[i], strerror(errno));
            continue;
        }

        if (readed == 0)
        {
            ErrorF("playback: FIFO %d gave EOF\n", data->fifo_fds[i]);
            close(data->fifo_fds[i]);
            data->fifo_fds[i] = -1;
            data->inodes[i] = 0;
            continue;
        }

        if (readed > max_readed)
            max_readed = readed;

        for (s = 0; s < readed / sizeof(int16_t); ++s)
        {
            /* FIXME: Ehhh, this'd be better as floats. With this algorithm,
             * samples mixed after being clipped will have undue weight. But
             * if we're clipping, then we're distorted anyway, so whatever. */
            if (out_buf[s] + buf[s] > INT16_MAX)
                out_buf[s] = INT16_MAX;
            else if (out_buf[s] + buf[s] < -INT16_MAX)
                out_buf[s] = -INT16_MAX;
            else
                out_buf[s] += buf[s];
        }
    }

    free(buf);

    if (!max_readed)
        return 0;

    data->valid_bytes = min(data->valid_bytes + max_readed,
            data->buffer_bytes);

    data->write_offs += max_readed;
    data->write_offs %= data->buffer_bytes;

    ++data->fed;

    return max_readed;
}

static int
scan_fifos (struct audio_data *data, const char *dirname)
{
    DIR *dir;
    struct dirent *ent;
    int i;

    dir = opendir(dirname);
    if (!dir)
    {
        ErrorF("playback: failed to open FIFO directory '%s': %s\n", dirname, strerror(errno));
        return 1;
    }

    while ((ent = readdir(dir)))
    {
        char path[PATH_MAX];

        if (ent->d_name[0] == '.')
            /* skip dot-files */
            continue;

        for (i = 0; i < MAX_FIFOS; ++i)
            if (ent->d_ino == data->inodes[i])
                break;
        if (i < MAX_FIFOS)
            /* file already open */
            continue;

        for (i = 0; i < MAX_FIFOS; ++i)
            if (data->fifo_fds[i] < 0)
                break;
        if (i == MAX_FIFOS)
        {
            static int once = 0;
            if (!once)
            {
                ErrorF("playback: Too many FIFOs already open\n");
                ++once;
            }
            closedir(dir);
            return 0;
        }

        if (snprintf(path, sizeof(path), "%s/%s", dirname, ent->d_name) >= sizeof(path)) {
            ErrorF("playback: FIFO filename is too long - truncated into %s", path);
        }

        data->fifo_fds[i] = open(path, O_RDONLY | O_RSYNC | O_NONBLOCK);
        if (data->fifo_fds[i] < 0)
        {
            ErrorF("playback: open FIFO '%s' failed: %s\n", path, strerror(errno));
            continue;
        }
        ErrorF("playback: opened FIFO '%s' as %d\n", path, data->fifo_fds[i]);

        data->inodes[i] = ent->d_ino;
    }

    closedir(dir);

    return 0;
}

static void *
audio_thread_main (void *p)
{
    qxl_screen_t *qxl = p;
    int i;
    struct audio_data data;
    int freq = SPICE_INTERFACE_PLAYBACK_FREQ;

    memset(&data, 0, sizeof(data));
    for (i = 0; i < MAX_FIFOS; ++i)
        data.fifo_fds[i] = -1;

#if SPICE_INTERFACE_PLAYBACK_MAJOR > 1 || SPICE_INTERFACE_PLAYBACK_MINOR >= 3
    freq = spice_server_get_best_playback_rate(&qxl->playback_sin);
#endif
    data.period_frames = freq * PERIOD_MS / 1000;


    data.frame_bytes = sizeof(int16_t) * SPICE_INTERFACE_PLAYBACK_CHAN;

    data.period_bytes = data.period_frames * data.frame_bytes;
    data.buffer_bytes = data.period_bytes * BUFFER_PERIODS;
    data.buffer = malloc(data.buffer_bytes);
    memset(data.buffer, 0, data.buffer_bytes);

    spice_server_playback_start(&qxl->playback_sin);

    gettimeofday(&data.last_read_time, NULL);

    while (1)
    {
        struct timeval end, diff, period_tv;

        if (scan_fifos(&data, qxl->playback_fifo_dir))
            goto cleanup;

        while (data.fed < BUFFER_PERIODS)
        {
            if (!read_from_fifos(&data))
                break;

            while (data.valid_bytes)
            {
                int to_copy_bytes;
                uint32_t read_offs;

                if (!data.spice_buffer)
                {
                    uint32_t chunk_frames;
                    spice_server_playback_get_buffer(&qxl->playback_sin, (uint32_t**)&data.spice_buffer, &chunk_frames);
                    data.spice_buffer_bytes = chunk_frames * data.frame_bytes;
                }
                if (!data.spice_buffer)
                    break;

                if (data.valid_bytes > data.write_offs)
                {
                    read_offs = data.buffer_bytes + data.write_offs - data.valid_bytes;
                    to_copy_bytes = min(data.buffer_bytes - read_offs,
                            data.spice_buffer_bytes - data.spice_write_offs);
                }
                else
                {
                    read_offs = data.write_offs - data.valid_bytes;
                    to_copy_bytes = min(data.valid_bytes,
                            data.spice_buffer_bytes - data.spice_write_offs);
                }

                memcpy(data.spice_buffer + data.spice_write_offs,
                        data.buffer + read_offs, to_copy_bytes);

                data.valid_bytes -= to_copy_bytes;

                data.spice_write_offs += to_copy_bytes;

                if (data.spice_write_offs >= data.spice_buffer_bytes)
                {
                    spice_server_playback_put_samples(&qxl->playback_sin, (uint32_t*)data.spice_buffer);
                    data.spice_buffer = NULL;
                    data.spice_buffer_bytes = data.spice_write_offs = 0;
                }
            }
        }

        period_tv.tv_sec = 0;
        period_tv.tv_usec = PERIOD_MS * 1000;

        usleep(period_tv.tv_usec);

        gettimeofday(&end, NULL);

        timersub(&end, &data.last_read_time, &diff);

        while (data.fed &&
                (diff.tv_sec > 0 || diff.tv_usec >= period_tv.tv_usec))
        {
            timersub(&diff, &period_tv, &diff);

            --data.fed;

            timeradd(&data.last_read_time, &period_tv, &data.last_read_time);
        }

        if (!data.fed)
            data.last_read_time = end;
    }

cleanup:
    if (data.spice_buffer)
    {
        memset(data.spice_buffer, 0, data.spice_buffer_bytes - data.spice_write_offs);
        spice_server_playback_put_samples(&qxl->playback_sin, (uint32_t*)data.spice_buffer);
        data.spice_buffer = NULL;
        data.spice_buffer_bytes = data.spice_write_offs = 0;
    }

    free(data.buffer);

    spice_server_playback_stop(&qxl->playback_sin);

    return NULL;
}

static const SpicePlaybackInterface playback_sif = {
    {
        SPICE_INTERFACE_PLAYBACK,
        "playback",
        SPICE_INTERFACE_PLAYBACK_MAJOR,
        SPICE_INTERFACE_PLAYBACK_MINOR
    }
};

int
qxl_add_spice_playback_interface (qxl_screen_t *qxl)
{
    int ret;

    if (qxl->playback_fifo_dir[0] == 0)
    {
        ErrorF("playback: no audio FIFO directory, audio is disabled\n");
        return 0;
    }

    qxl->playback_sin.base.sif = &playback_sif.base;
    ret = spice_server_add_interface(qxl->spice_server, &qxl->playback_sin.base);
    if (ret < 0)
        return errno;

#if SPICE_INTERFACE_PLAYBACK_MAJOR > 1 || SPICE_INTERFACE_PLAYBACK_MINOR >= 3
    spice_server_set_playback_rate(&qxl->playback_sin,
            spice_server_get_best_playback_rate(&qxl->playback_sin));
#else
    /* disable CELT */
    ret = spice_server_set_playback_compression(qxl->spice_server, 0);
    if (ret < 0)
        return errno;

#endif

    ret = pthread_create(&qxl->audio_thread, NULL, &audio_thread_main, qxl);
    if (ret < 0)
        return errno;

    return 0;
}
