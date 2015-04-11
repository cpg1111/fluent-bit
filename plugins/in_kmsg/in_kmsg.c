/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <inttypes.h>

#include <msgpack.h>
#include <fluent-bit/in_kmsg.h>
#include <fluent-bit/flb_input.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_engine.h>

/*
 * Note: Functions timeval_diff() and in_kmsg_boot_time() are based
 * on syslog-ng-3.5 source code.
 */
static inline uint64_t timeval_diff(struct timeval *t1, struct timeval *t2)
{
    return ((uint64_t) t1->tv_sec - (uint64_t) t2->tv_sec) * KMSG_USEC_PER_SEC +
        ((uint64_t) t1->tv_usec - (uint64_t) t2->tv_usec);
}

static int boot_time(struct timeval *boot_time)
{
    int fd, pos = 0;
    int bytes;
    uint64_t tdiff;
    char buf[256];
    struct timeval curr_time;

    fd = open("/proc/uptime", O_RDONLY);
    if (fd == -1) {
        return -1;
    }

    bytes = read(fd, buf, sizeof(buf));
    if (bytes <= 0) {
        close(fd);
        return -1;
    }

    close(fd);
    gettimeofday(&curr_time, NULL);

    /* Read the seconds part */
    while (pos < bytes && buf[pos] != '.') {
        if (isdigit(buf[pos])) {
            boot_time->tv_sec = boot_time->tv_sec * 10 + ((buf[pos]) - '0');
        }
        else {
            boot_time->tv_sec = 0;
            return 0;
        }
        pos++;
    }
    pos++;

    /* Then the microsecond part */
    while (pos < bytes && buf[pos] != ' ') {
        if (isdigit(buf[pos])) {
            boot_time->tv_usec = boot_time->tv_usec * 10 + ((buf[pos]) - '0');
        }
        else {
            boot_time->tv_sec = 0;
            boot_time->tv_usec = 0;
            return 0;
        }
        pos++;
    }

    tdiff = timeval_diff(&curr_time, boot_time);
    boot_time->tv_sec  = tdiff / KMSG_USEC_PER_SEC;
    boot_time->tv_usec = tdiff % KMSG_USEC_PER_SEC;

    return 0;
}

void *in_kmsg_flush(void *in_context, int *size)
{
    int i;
    char *buf;
    msgpack_packer pck;
    msgpack_sbuffer sbuf;
    struct kmsg_line *entry;
    struct flb_in_kmsg_config *ctx = in_context;

    /* initialize buffers */
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pck, &sbuf, msgpack_sbuffer_write);

    msgpack_pack_array(&pck, 2);

    /* Tag */
    msgpack_pack_raw(&pck, ctx->tag_len);
    msgpack_pack_raw_body(&pck, ctx->tag, ctx->tag_len);

    /* Primary Array: ['TAG', [ */
    msgpack_pack_array(&pck, ctx->buffer_id);

    /* Pack each data_array entry */
    for (i = 0; i < ctx->buffer_id; i++) {
        entry = &ctx->buffer[i];

        uint64_t t = ctx->boot_time.tv_sec + entry->tv.tv_sec;

        msgpack_pack_array(&pck, 2);
        msgpack_pack_uint64(&pck, t);

        msgpack_pack_map(&pck, 3);

        /* Priority */
        msgpack_pack_raw(&pck, 8);
        msgpack_pack_raw_body(&pck, "priority", 8);
        msgpack_pack_char(&pck, entry->priority);

        /* Sequence */
        msgpack_pack_raw(&pck, 8);
        msgpack_pack_raw_body(&pck, "sequence", 8);
        msgpack_pack_uint64(&pck, entry->sequence);

        /* Time: FIXME */

        /* Data message (line) */
        msgpack_pack_raw(&pck, 3);
        msgpack_pack_raw_body(&pck, "msg", 3);
        msgpack_pack_raw(&pck, entry->line_len);
        msgpack_pack_raw_body(&pck, entry->line, entry->line_len);
    }

    *size = sbuf.size;
    buf = malloc(sbuf.size);
    memcpy(buf, sbuf.data, sbuf.size);
    msgpack_sbuffer_destroy(&sbuf);

    ctx->buffer_id = 0;

    return buf;
}

static inline int process_line(char *line, struct flb_in_kmsg_config *ctx)
{
    uint64_t val;
    char *p = line;
    char *end = NULL;
    struct kmsg_line *buf;

    /* Increase buffer position */
    ctx->buffer_id++;

    errno = 0;
    val = strtol(p, &end, 10);
    if ((errno == ERANGE && (val == INT_MAX || val == INT_MIN))
        || (errno != 0 && val == 0)) {
        goto fail;
    }

    /* Lookup */
    buf = &ctx->buffer[ctx->buffer_id];

    /* Priority */
    buf->priority = FLB_LOG_PRI(val);

    /* Sequence */
    p = strchr(p, ',');
    p++;

    val = strtol(p, &end, 10);
    if ((errno == ERANGE && (val == INT_MAX || val == INT_MIN))
        || (errno != 0 && val == 0)) {
        goto fail;
    }

    buf->sequence = val;
    p = ++end;

    /* Timestamp */
    val = strtol(p, &end, 10);
    if ((errno == ERANGE && (val == INT_MAX || val == INT_MIN))
        || (errno != 0 && val == 0)) {
        goto fail;
    }
    buf->tv.tv_sec = val;

    if (*end == '.') {
        p = ++end;
        val = strtol(p, &end, 10);
        if ((errno == ERANGE && (val == INT_MAX || val == INT_MIN))
            || (errno != 0 && val == 0)) {
            goto fail;
        }
        buf->tv.tv_usec = val;
    }
    else {
        buf->tv.tv_usec = 0;
    }

    /* Now process the human readable message */
    p = strchr(p, ';');
    if (!p) {
        goto fail;
    }
    p++;

    buf->line_len = strlen(p);
    strncpy(buf->line, p, buf->line_len);
    buf->line[buf->line_len] = '\0';

    flb_debug("pri=%i seq=%" PRIu64 " sec=%ld usec=%ld '%s'",
              buf->priority,
              buf->sequence,
              (long int) buf->tv.tv_sec,
              (long int) buf->tv.tv_usec,
              (const char *) buf->line);

    return 0;


 fail:
    ctx->buffer_id--;
    return -1;
}

/* Callback invoked after setup but before to join the main loop */
int in_kmsg_pre_run(void *in_context, struct flb_config *config)
{
    struct flb_in_kmsg_config *ctx = in_context;

    /* Tag */
    ctx->tag_len = snprintf(ctx->tag, sizeof(ctx->tag) - 1,
                            "%s.kmsg", config->tag);
    if (ctx->tag_len == -1) {
        flb_utils_error_c("Could not set custom tag on kmsg input plugin");
    }

    return 0;
}

/* Callback triggered when some Kernel Log buffer msgs are available */
int in_kmsg_collect(struct flb_config *config, void *in_context)
{
    int bytes;
    char line[2024];
    struct flb_in_kmsg_config *ctx = in_context;

    bytes = read(ctx->fd, line, sizeof(line) -1);
    if (bytes == -1) {
        if (errno == -EPIPE) {
            return -1;
        }
        return 0;
    }
    /* Always set a delimiter to avoid buffer trash */
    line[bytes - 1] = '\0';

    /* Check if our buffer is full */
    if (ctx->buffer_id + 1 == KMSG_BUFFER_SIZE) {
        flb_engine_flush(config, &in_kmsg_plugin, NULL);
    }

    /* Process and enqueue the received line */
    process_line(line, ctx);
    return 0;
}

/* Init kmsg input */
int in_kmsg_init(struct flb_config *config)
{
    int fd;
    int ret;
    struct flb_in_kmsg_config *ctx;

    ctx = calloc(1, sizeof(struct flb_in_kmsg_config));
    if (!ctx) {
        perror("calloc");
        return -1;
    }

    /* open device */
    fd = open(FLB_KMSG_DEV, O_RDONLY);
    if (fd == -1) {
        perror("open");
        flb_utils_error_c("Could not open kernel log buffer on kmsg plugin");
    }
    ctx->fd = fd;

    /* get the system boot time */
    ret = boot_time(&ctx->boot_time);
    if (ret == -1) {
        flb_utils_error_c("Could not get system boot time for kmsg input plugin");
    }

    /* set context */
    ret = flb_input_set_context("kmsg", ctx, config);
    if (ret == -1) {
        flb_utils_error_c("Could not set configuration for kmsg input plugin");
    }

    /* Set our collector based on a file descriptor event */
    ret = flb_input_set_collector_event("kmsg",
                                        in_kmsg_collect,
                                        ctx->fd,
                                        config);
    if (ret == -1) {
        flb_utils_error_c("Could not set collector for kmsg input plugin");
    }

    return 0;
}

/* Plugin reference */
struct flb_input_plugin in_kmsg_plugin = {
    .name         = "kmsg",
    .description  = "Kernel Log Buffer",
    .cb_init      = in_kmsg_init,
    .cb_pre_run   = in_kmsg_pre_run,
    .cb_collect   = in_kmsg_collect,
    .cb_flush_buf = in_kmsg_flush
};