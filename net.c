/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <poll.h>
#include <limits.h>
#include <stdlib.h>

#include "net.h"
#include "sds.h"

/* Defined in himongo.c */
void __mongoSetError(mongoContext *c, int type, const char *str);

static void mongoContextCloseFd(mongoContext *c) {
    if (c && c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static void __mongoSetErrorFromErrno(mongoContext *c, int type, const char *prefix) {
    int errorno = errno;  /* snprintf() may change errno */
    char buf[128] = { 0 };
    size_t len = 0;

    if (prefix != NULL)
        len = snprintf(buf,sizeof(buf),"%s: ",prefix);
    __mongo_strerror_r(errorno, (char *)(buf + len), sizeof(buf) - len);
    __mongoSetError(c,type,buf);
}

static int mongoSetReuseAddr(mongoContext *c) {
    int on = 1;
    if (setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        __mongoSetErrorFromErrno(c,MONGO_ERR_IO,NULL);
        mongoContextCloseFd(c);
        return MONGO_ERR;
    }
    return MONGO_OK;
}

static int mongoCreateSocket(mongoContext *c, int type) {
    int s;
    if ((s = socket(type, SOCK_STREAM, 0)) == -1) {
        __mongoSetErrorFromErrno(c,MONGO_ERR_IO,NULL);
        return MONGO_ERR;
    }
    c->fd = s;
    if (type == AF_INET) {
        if (mongoSetReuseAddr(c) == MONGO_ERR) {
            return MONGO_ERR;
        }
    }
    return MONGO_OK;
}

static int mongoSetBlocking(mongoContext *c, int blocking) {
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(c->fd, F_GETFL)) == -1) {
        __mongoSetErrorFromErrno(c,MONGO_ERR_IO,"fcntl(F_GETFL)");
        mongoContextCloseFd(c);
        return MONGO_ERR;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (fcntl(c->fd, F_SETFL, flags) == -1) {
        __mongoSetErrorFromErrno(c,MONGO_ERR_IO,"fcntl(F_SETFL)");
        mongoContextCloseFd(c);
        return MONGO_ERR;
    }
    return MONGO_OK;
}

int mongoKeepAlive(mongoContext *c, int interval) {
    int val = 1;
    int fd = c->fd;

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1){
        __mongoSetError(c,MONGO_ERR_OTHER,strerror(errno));
        return MONGO_ERR;
    }

    val = interval;

#ifdef _OSX
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &val, sizeof(val)) < 0) {
        __mongoSetError(c,MONGO_ERR_OTHER,strerror(errno));
        return MONGO_ERR;
    }
#else
#if defined(__GLIBC__) && !defined(__FreeBSD_kernel__)
    val = interval;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        __mongoSetError(c,MONGO_ERR_OTHER,strerror(errno));
        return MONGO_ERR;
    }

    val = interval/3;
    if (val == 0) val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        __mongoSetError(c,MONGO_ERR_OTHER,strerror(errno));
        return MONGO_ERR;
    }

    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        __mongoSetError(c,MONGO_ERR_OTHER,strerror(errno));
        return MONGO_ERR;
    }
#endif
#endif

    return MONGO_OK;
}

static int mongoSetTcpNoDelay(mongoContext *c) {
    int yes = 1;
    if (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        __mongoSetErrorFromErrno(c,MONGO_ERR_IO,"setsockopt(TCP_NODELAY)");
        mongoContextCloseFd(c);
        return MONGO_ERR;
    }
    return MONGO_OK;
}

#define __MAX_MSEC (((LONG_MAX) - 999) / 1000)

static int mongoContextTimeoutMsec(mongoContext *c, long *result)
{
    const struct timeval *timeout = c->timeout;
    long msec = -1;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        if (timeout->tv_usec > 1000000 || timeout->tv_sec > __MAX_MSEC) {
            *result = msec;
            return MONGO_ERR;
        }

        msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);

        if (msec < 0 || msec > INT_MAX) {
            msec = INT_MAX;
        }
    }

    *result = msec;
    return MONGO_OK;
}

static int mongoContextWaitReady(mongoContext *c, long msec) {
    struct pollfd   wfd[1];

    wfd[0].fd     = c->fd;
    wfd[0].events = POLLOUT;

    if (errno == EINPROGRESS) {
        int res;

        if ((res = poll(wfd, 1, msec)) == -1) {
            __mongoSetErrorFromErrno(c, MONGO_ERR_IO, "poll(2)");
            mongoContextCloseFd(c);
            return MONGO_ERR;
        } else if (res == 0) {
            errno = ETIMEDOUT;
            __mongoSetErrorFromErrno(c,MONGO_ERR_IO,NULL);
            mongoContextCloseFd(c);
            return MONGO_ERR;
        }

        if (mongoCheckSocketError(c) != MONGO_OK)
            return MONGO_ERR;

        return MONGO_OK;
    }

    __mongoSetErrorFromErrno(c,MONGO_ERR_IO,NULL);
    mongoContextCloseFd(c);
    return MONGO_ERR;
}

int mongoCheckSocketError(mongoContext *c) {
    int err = 0;
    socklen_t errlen = sizeof(err);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
        __mongoSetErrorFromErrno(c,MONGO_ERR_IO,"getsockopt(SO_ERROR)");
        return MONGO_ERR;
    }

    if (err) {
        errno = err;
        __mongoSetErrorFromErrno(c,MONGO_ERR_IO,NULL);
        return MONGO_ERR;
    }

    return MONGO_OK;
}

int mongoContextSetTimeout(mongoContext *c, const struct timeval tv) {
    if (setsockopt(c->fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv)) == -1) {
        __mongoSetErrorFromErrno(c,MONGO_ERR_IO,"setsockopt(SO_RCVTIMEO)");
        return MONGO_ERR;
    }
    if (setsockopt(c->fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv)) == -1) {
        __mongoSetErrorFromErrno(c,MONGO_ERR_IO,"setsockopt(SO_SNDTIMEO)");
        return MONGO_ERR;
    }
    return MONGO_OK;
}

static int _mongoContextConnectTcp(mongoContext *c, const char *addr, int port,
                                   const struct timeval *timeout,
                                   const char *source_addr) {
    int s, rv, n;
    char _port[6];  /* strlen("65535"); */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;
    int blocking = (c->flags & MONGO_BLOCK);
    int reuseaddr = (c->flags & MONGO_REUSEADDR);
    int reuses = 0;
    long timeout_msec = -1;

    servinfo = NULL;
    c->connection_type = MONGO_CONN_TCP;
    c->tcp.port = port;

    /* We need to take possession of the passed parameters
     * to make them reusable for a reconnect.
     * We also carefully check we don't free data we already own,
     * as in the case of the reconnect method.
     *
     * This is a bit ugly, but atleast it works and doesn't leak memory.
     **/
    if (c->tcp.host != addr) {
        if (c->tcp.host)
            free(c->tcp.host);

        c->tcp.host = strdup(addr);
    }

    if (timeout) {
        if (c->timeout != timeout) {
            if (c->timeout == NULL)
                c->timeout = malloc(sizeof(struct timeval));

            memcpy(c->timeout, timeout, sizeof(struct timeval));
        }
    } else {
        if (c->timeout)
            free(c->timeout);
        c->timeout = NULL;
    }

    if (mongoContextTimeoutMsec(c, &timeout_msec) != MONGO_OK) {
        __mongoSetError(c, MONGO_ERR_IO, "Invalid timeout specified");
        goto error;
    }

    if (source_addr == NULL) {
        free(c->tcp.source_addr);
        c->tcp.source_addr = NULL;
    } else if (c->tcp.source_addr != source_addr) {
        free(c->tcp.source_addr);
        c->tcp.source_addr = strdup(source_addr);
    }

    snprintf(_port, 6, "%d", port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* Try with IPv6 if no IPv4 address was found. We do it in this order since
     * in a Mongo client you can't afford to test if you have IPv6 connectivity
     * as this would add latency to every connect. Otherwise a more sensible
     * route could be: Use IPv6 if both addresses are available and there is IPv6
     * connectivity. */
    if ((rv = getaddrinfo(c->tcp.host,_port,&hints,&servinfo)) != 0) {
         hints.ai_family = AF_INET6;
         if ((rv = getaddrinfo(addr,_port,&hints,&servinfo)) != 0) {
            __mongoSetError(c,MONGO_ERR_OTHER,gai_strerror(rv));
            return MONGO_ERR;
        }
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
addrretry:
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;

        c->fd = s;
        if (mongoSetBlocking(c,0) != MONGO_OK)
            goto error;
        if (c->tcp.source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(c->tcp.source_addr, NULL, &hints, &bservinfo)) != 0) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't get addr: %s",gai_strerror(rv));
                __mongoSetError(c,MONGO_ERR_OTHER,buf);
                goto error;
            }

            if (reuseaddr) {
                n = 1;
                if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*) &n,
                               sizeof(n)) < 0) {
                    goto error;
                }
            }

            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s,b->ai_addr,b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't bind socket: %s",strerror(errno));
                __mongoSetError(c,MONGO_ERR_OTHER,buf);
                goto error;
            }
        }
        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            if (errno == EHOSTUNREACH) {
                mongoContextCloseFd(c);
                continue;
            } else if (errno == EINPROGRESS && !blocking) {
                /* This is ok. */
            } else if (errno == EADDRNOTAVAIL && reuseaddr) {
                if (++reuses >= MONGO_CONNECT_RETRIES) {
                    goto error;
                } else {
                    mongoContextCloseFd(c);
                    goto addrretry;
                }
            } else {
                if (mongoContextWaitReady(c,timeout_msec) != MONGO_OK)
                    goto error;
            }
        }
        if (blocking && mongoSetBlocking(c,1) != MONGO_OK)
            goto error;
        if (mongoSetTcpNoDelay(c) != MONGO_OK)
            goto error;

        c->flags |= MONGO_CONNECTED;
        rv = MONGO_OK;
        goto end;
    }
    if (p == NULL) {
        char buf[128];
        snprintf(buf,sizeof(buf),"Can't create socket: %s",strerror(errno));
        __mongoSetError(c,MONGO_ERR_OTHER,buf);
        goto error;
    }

error:
    rv = MONGO_ERR;
end:
    freeaddrinfo(servinfo);
    return rv;  // Need to return MONGO_OK if alright
}

int mongoContextConnectTcp(mongoContext *c, const char *addr, int port,
                           const struct timeval *timeout) {
    return _mongoContextConnectTcp(c, addr, port, timeout, NULL);
}

int mongoContextConnectBindTcp(mongoContext *c, const char *addr, int port,
                               const struct timeval *timeout,
                               const char *source_addr) {
    return _mongoContextConnectTcp(c, addr, port, timeout, source_addr);
}

int mongoContextConnectUnix(mongoContext *c, const char *path, const struct timeval *timeout) {
    int blocking = (c->flags & MONGO_BLOCK);
    struct sockaddr_un sa;
    long timeout_msec = -1;

    if (mongoCreateSocket(c,AF_LOCAL) < 0)
        return MONGO_ERR;
    if (mongoSetBlocking(c,0) != MONGO_OK)
        return MONGO_ERR;

    c->connection_type = MONGO_CONN_UNIX;
    if (c->unix_sock.path != path)
        c->unix_sock.path = strdup(path);

    if (timeout) {
        if (c->timeout != timeout) {
            if (c->timeout == NULL)
                c->timeout = malloc(sizeof(struct timeval));

            memcpy(c->timeout, timeout, sizeof(struct timeval));
        }
    } else {
        if (c->timeout)
            free(c->timeout);
        c->timeout = NULL;
    }

    if (mongoContextTimeoutMsec(c,&timeout_msec) != MONGO_OK)
        return MONGO_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (connect(c->fd, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            if (mongoContextWaitReady(c,timeout_msec) != MONGO_OK)
                return MONGO_ERR;
        }
    }

    /* Reset socket to be blocking after connect(2). */
    if (blocking && mongoSetBlocking(c,1) != MONGO_OK)
        return MONGO_ERR;

    c->flags |= MONGO_CONNECTED;
    return MONGO_OK;
}
