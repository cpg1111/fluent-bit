/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2016 Treasure Data Inc.
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

/*
 * FLB_IO
 * ======
 * This interface is used by the output plugins which needs to write over
 * the network in plain communication or through the TLS support. When dealing
 * with network operation there are a few things to keep in mind:
 *
 * - TCP hosts can be down.
 * - Network can be slow.
 * - If the amount of data to flush requires multiple 'write' operations, we
 *   should not block the main thread, instead use event-driven mechanism to
 *   write when is possible.
 *
 * Output plugins that flag their selfs with FLB_OUTPUT_TCP or FLB_OUTPUT_TLS
 * can take advante of this interface.
 *
 * The workflow to use this is the following:
 *
 * - A connection and data flow requires an flb_io_upstream context.
 * - We write/read data through the flb_io_write()/flb_io_read() interfaces.
 *
 * Note that Upstreams context may define how network operations will work,
 * basically synchronous or asynchronous (non-blocking).
 *
 * When Fluent Bit is set with config->flush_mode = FLB_FLUSH_PTHREADS, all
 * operations will work in blocking (synchronous) mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_config.h>
#include <fluent-bit/flb_io.h>
#include <fluent-bit/flb_io_tls.h>
#include <fluent-bit/flb_tls.h>
#include <fluent-bit/flb_upstream.h>

#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_macros.h>
#include <fluent-bit/flb_network.h>
#include <fluent-bit/flb_engine.h>
#include <fluent-bit/flb_thread.h>

FLB_INLINE int flb_io_net_connect(struct flb_upstream_conn *u_conn,
                                  struct flb_thread *th)
{
    int fd;
    int ret;
    int error = 0;
    uint32_t mask;
    socklen_t len = sizeof(error);
    struct flb_upstream *u = u_conn->u;

    if (u_conn->fd > 0) {
        close(u_conn->fd);
    }

    /* Create the socket */
    fd = flb_net_socket_create(AF_INET, FLB_FALSE);
    if (fd == -1) {
        flb_error("[io] could not create socket");
        return -1;
    }
    u_conn->fd = fd;

    /*
     * If we use co-routines flushing method, make sure socket
     * operations are asynchronous
     */
    if (u->flags & FLB_IO_ASYNC) {
        flb_net_socket_nonblocking(u_conn->fd);
    }

    flb_net_socket_tcp_nodelay(fd);

    /* Start the connection */
    ret = flb_net_tcp_fd_connect(fd, u->tcp_host, u->tcp_port);
    if (ret == -1) {
        /* In blocking mode connect() fails right away */
        if ((u->flags & FLB_IO_ASYNC) == 0) {
            close(fd);
            return -1;
        }

        if (errno == EINPROGRESS) {
            flb_trace("[upstream] connection in process");
        }
        else {
            close(fd);
            return -1;
        }

        MK_EVENT_NEW(&u_conn->event);
        u_conn->thread = th;
        ret = mk_event_add(u->evl,
                           fd,
                           FLB_ENGINE_EV_THREAD,
                           MK_EVENT_WRITE, &u_conn->event);
        if (ret == -1) {
            /*
             * If we failed here there no much that we can do, just
             * let the caller we failed
             */
            close(fd);
            return -1;
        }

        /*
         * Return the control to the parent caller, we need to wait for
         * the event loop to get back to us.
         */
        flb_thread_yield(th, FLB_FALSE);

        /* Save the mask before the event handler do a reset */
        mask = u_conn->event.mask;

        /* We got a notification, remove the event registered */
        ret = mk_event_del(u->evl, &u_conn->event);
        if (ret == -1) {
            flb_error("[io] connect event handler error");
            close(fd);
            return -1;
        }

        /* Check the connection status */
        if (mask & MK_EVENT_WRITE) {
            ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            if (ret == -1) {
                flb_error("[io] could not validate socket status");
                close(fd);
                return -1;
            }

            if (error != 0) {
                /* Connection is broken, not much to do here */
                flb_error("[io] TCP connection failed: %s:%i",
                          u->tcp_host, u->tcp_port);
                close(fd);
                return -1;
            }
        }
        else {
            flb_error("[io] TCP connection, unexpected error: %s:%i",
                      u->tcp_host, u->tcp_port);
            close(fd);
            return -1;
        }
    }

#ifdef FLB_HAVE_TLS
    /* Check if TLS was enabled, if so perform the handshakee */
    if (u_conn->u->flags & FLB_IO_TLS) {
        ret = net_io_tls_handshake(u_conn, th);
        if (ret != 0) {
            close(fd);
            return -1;
        }
    }
#endif

    u_conn->connect_count++;
    flb_trace("[io] connection OK");

    return 0;
}

static int net_io_write(struct flb_upstream_conn *u_conn,
                        void *data, size_t len, size_t *out_len)
{
    int ret;
    int tries = 0;
    size_t total = 0;

    if (u_conn->fd <= 0) {
        struct flb_thread *th;
        th = (struct flb_thread *) pthread_getspecific(flb_thread_key);
        ret = flb_io_net_connect(u_conn, th);
        if (ret == -1) {
            return -1;
        }
    }

    while (total < len) {
        ret = write(u_conn->fd, data + total, len - total);
        if (ret == -1) {
            if (errno == EAGAIN) {
                /*
                 * FIXME: for now we are handling this in a very lazy way,
                 * just sleep for a second and retry (for a max of 30 tries).
                 */
                sleep(1);
                tries++;

                if (tries == 30) {
                    return -1;
                }
                continue;
            }
            return -1;
        }
        tries = 0;
        total += ret;
    }

    *out_len = total;
    return total;
}

/*
 * Perform Async socket write(2) operations. This function depends on a
 * maine event-loop and the co-routines interface to yield/resume once
 * sockets are ready to continue.
 *
 * Intentionally we register/de-register the socket file descriptor from
 * the event loop each time when we require to do some work.
 */
static FLB_INLINE int net_io_write_async(struct flb_thread *th,
                                         struct flb_upstream_conn *u_conn,
                                         void *data, size_t len, size_t *out_len)
{
    int ret = 0;
    int error;
    ssize_t bytes;
    size_t total = 0;
    size_t send;
    socklen_t slen = sizeof(error);
    struct flb_upstream *u = u_conn->u;

 retry:
    error = 0;

    if (len - total > 524288) {
        send = 524288;
    }
    else {
        send = (len - total);
    }
    bytes = write(u_conn->fd, data + total, send);

#ifdef FLB_HAVE_TRACE
    if (bytes > 0) {
        flb_trace("[io thread=%p] [fd %i] write_async(2)=%d (%lu/%lu)",
                  th, u_conn->fd, bytes, total + bytes, len);
    }
    else {
        flb_trace("[io thread=%p] [fd %i] write_async(2)=%d (%lu/%lu)",
                  th, u_conn->fd, bytes, total, len);
    }
#endif

    if (bytes == -1) {
        if (errno == EAGAIN) {
            MK_EVENT_NEW(&u_conn->event);
            u_conn->thread = th;

            ret = mk_event_add(u->evl,
                               u_conn->fd,
                               FLB_ENGINE_EV_THREAD,
                               MK_EVENT_WRITE, &u_conn->event);
            if (ret == -1) {
                /*
                 * If we failed here there no much that we can do, just
                 * let the caller we failed
                 */
                return -1;
            }

            /*
             * Return the control to the parent caller, we need to wait for
             * the event loop to get back to us.
             */
            flb_thread_yield(th, FLB_FALSE);

            /* We got a notification, remove the event registered */
            ret = mk_event_del(u->evl, &u_conn->event);
            if (ret == -1) {
                return -1;
            }

            /* Check the connection status */
            if (u_conn->event.mask & MK_EVENT_WRITE) {
                ret = getsockopt(u_conn->fd, SOL_SOCKET, SO_ERROR, &error, &slen);
                if (ret == -1) {
                    flb_error("[io] could not validate socket status");
                    return -1;
                }

                if (error != 0) {
                    /* Connection is broken, not much to do here */
                    flb_error("[io] TCP connection failed: %s:%i",
                              u->tcp_host, u->tcp_port);
                    return -1;
                }

                MK_EVENT_NEW(&u_conn->event);
                goto retry;
            }
            else {
                return -1;
            }

        }
        else {
            return -1;
        }
    }

    /* Update statistics */
    //flb_stats_update(out->stats_fd, ret, 0);

    /* Update counters */
    total += bytes;
    if (total < len) {
        if (u_conn->event.status == MK_EVENT_NONE) {
            u_conn->event.mask = MK_EVENT_EMPTY;
            u_conn->thread = th;
            ret = mk_event_add(u->evl,
                               u_conn->fd,
                               FLB_ENGINE_EV_THREAD,
                               MK_EVENT_WRITE, &u_conn->event);
            if (ret == -1) {
                /*
                 * If we failed here there no much that we can do, just
                 * let the caller we failed
                 */
                return -1;
            }
        }
        flb_thread_yield(th, MK_FALSE);
        goto retry;
    }

    if (u_conn->event.status & MK_EVENT_REGISTERED) {
        /* We got a notification, remove the event registered */
        ret = mk_event_del(u->evl, &u_conn->event);
        assert(ret == 0);
    }

    *out_len = total;
    return bytes;
}

static ssize_t net_io_read(struct flb_upstream_conn *u_conn,
                           void *buf, size_t len)
{
    int ret;

    ret = read(u_conn->fd, buf, len);
    if (ret == -1) {
        return -1;
    }

    return ret;
}

static FLB_INLINE ssize_t net_io_read_async(struct flb_thread *th,
                                            struct flb_upstream_conn *u_conn,
                                            void *buf, size_t len)
{
    int ret;
    struct flb_upstream *u = u_conn->u;

 retry_read:

    ret = read(u_conn->fd, buf, len);
    if (ret == -1) {
        if (errno == EAGAIN) {
            u_conn->thread = th;
            ret = mk_event_add(u->evl,
                               u_conn->fd,
                               FLB_ENGINE_EV_THREAD,
                               MK_EVENT_READ, &u_conn->event);
            if (ret == -1) {
                /*
                 * If we failed here there no much that we can do, just
                 * let the caller we failed
                 */
                close(u_conn->fd);
                return -1;
            }
            flb_thread_yield(th, MK_FALSE);
            goto retry_read;
        }
        return -1;
    }
    else if (ret <= 0) {
        return -1;
    }

    return ret;
}

/* Write data to an upstream connection/server */
int flb_io_net_write(struct flb_upstream_conn *u_conn, void *data,
                     size_t len, size_t *out_len)
{
    int ret = -1;
    struct flb_upstream *u = u_conn->u;

#if defined (FLB_HAVE_FLUSH_UCONTEXT) || defined (FLB_HAVE_FLUSH_LIBCO)
    struct flb_thread *th = pthread_getspecific(flb_thread_key);
    flb_trace("[io thread=%p] [net_write] trying %zd bytes",
              th, len);
#else
    void *th = NULL;
    flb_trace("[io] [net_write] trying %zd bytes", len);
#endif
    if (u->flags & FLB_IO_TCP) {
        if (u->flags & FLB_IO_ASYNC) {
            ret = net_io_write_async(th, u_conn, data, len, out_len);
        }
        else {
            ret = net_io_write(u_conn, data, len, out_len);
        }
    }
#ifdef FLB_HAVE_TLS
    else if (u->flags & FLB_IO_TLS) {
        ret = net_io_tls_write(th, u_conn, data, len, out_len);
    }
#endif
    if (ret == -1 && u_conn->fd > 0) {
        close(u_conn->fd);
        u_conn->fd = -1;
    }

#if defined (FLB_HAVE_FLUSH_UCONTEXT) || defined (FLB_HAVE_FLUSH_LIBCO)
    flb_trace("[io thread=%p] [net_write] ret=%i total=%lu/%lu",
              th, ret, *out_len, len);
#else
    flb_trace("[io] [net_write] ret=%i total=%i",
              ret, *out_len);
#endif
    return ret;
}

ssize_t flb_io_net_read(struct flb_upstream_conn *u_conn, void *buf, size_t len)
{
    int ret = -1;
    struct flb_upstream *u = u_conn->u;

#if defined (FLB_HAVE_FLUSH_UCONTEXT) || defined (FLB_HAVE_FLUSH_LIBCO)
    struct flb_thread *th = pthread_getspecific(flb_thread_key);
    flb_trace("[io thread=%p] [net_read] try up to %zd bytes",
              th, len);
#else
    void *th = NULL;
    flb_trace("[io] [net_read] try up to %zd bytes", len);
#endif

    if (u->flags & FLB_IO_TCP) {
        if (u->flags & FLB_IO_ASYNC) {
            ret = net_io_read_async(th, u_conn, buf, len);
        }
        else {
            ret = net_io_read(u_conn, buf, len);
        }
    }
#ifdef FLB_HAVE_TLS
    else if (u->flags & FLB_IO_TLS) {
        ret = net_io_tls_read(th, u_conn, buf, len);
    }
#endif

    flb_trace("[io thread=%p] [net_read] ret=%i", th, ret);
    return ret;
}
