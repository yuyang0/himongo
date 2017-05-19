#ifndef __HIMONGO_H
#define __HIMONGO_H
#include "read.h"
#include <stdarg.h> /* for va_list */
#include <sys/time.h> /* for struct timeval */
#include <stdint.h> /* uintXX_t, etc */
#include "sds.h" /* for sds */

#define HIMONGO_MAJOR 0
#define HIMONGO_MINOR 13
#define HIMONGO_PATCH 3
#define HIMONGO_SONAME 0.13

/* Connection type can be blocking or non-blocking and is set in the
 * least significant bit of the flags field in mongoContext. */
#define MONGO_BLOCK 0x1

/* Connection may be disconnected before being free'd. The second bit
 * in the flags field is set when the context is connected. */
#define MONGO_CONNECTED 0x2

/* The async API might try to disconnect cleanly and flush the output
 * buffer and read all subsequent replies before disconnecting.
 * This flag means no new commands can come in and the connection
 * should be terminated once all replies have been read. */
#define MONGO_DISCONNECTING 0x4

/* Flag specific to the async API which means that the context should be clean
 * up as soon as possible. */
#define MONGO_FREEING 0x8

/* Flag that is set when an async callback is executed. */
#define MONGO_IN_CALLBACK 0x10

/* Flag that is set when the async context has one or more subscriptions. */
#define MONGO_SUBSCRIBED 0x20

/* Flag that is set when monitor mode is active */
#define MONGO_MONITORING 0x40

/* Flag that is set when we should set SO_REUSEADDR before calling bind() */
#define MONGO_REUSEADDR 0x80

#define MONGO_KEEPALIVE_INTERVAL 15 /* seconds */

/* number of times we retry to connect in the case of EADDRNOTAVAIL and
 * SO_REUSEADDR is being used. */
#define MONGO_CONNECT_RETRIES  10

/* strerror_r has two completely different prototypes and behaviors
 * depending on system issues, so we need to operate on the error buffer
 * differently depending on which strerror_r we're using. */
#ifndef _GNU_SOURCE
/* "regular" POSIX strerror_r that does the right thing. */
#define __mongo_strerror_r(errno, buf, len)                                    \
    do {                                                                       \
        strerror_r((errno), (buf), (len));                                     \
    } while (0)
#else
/* "bad" GNU strerror_r we need to clean up after. */
#define __mongo_strerror_r(errno, buf, len)                                    \
    do {                                                                       \
        char *err_str = strerror_r((errno), (buf), (len));                     \
        /* If return value _isn't_ the start of the buffer we passed in,       \
         * then GNU strerror_r returned an internal static buffer and we       \
         * need to copy the result into our private buffer. */                 \
        if (err_str != (buf)) {                                                \
            strncpy((buf), err_str, ((len) - 1));                              \
            (buf)[(len)-1] = '\0';                                               \
        }                                                                      \
    } while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* This is the reply object returned by mongoCommand() */
typedef struct mongoReply {
    int type; /* MONGO_REPLY_* */
    long long integer; /* The integer when type is MONGO_REPLY_INTEGER */
    size_t len; /* Length of string */
    char *str; /* Used for both MONGO_REPLY_ERROR and MONGO_REPLY_STRING */
    size_t elements; /* number of elements, for MONGO_REPLY_ARRAY */
    struct mongoReply **element; /* elements vector for MONGO_REPLY_ARRAY */
} mongoReply;

mongoReader *mongoReaderCreate(void);

/* Function to free the reply objects himongo returns by default. */
void freeReplyObject(void *reply);

/* Functions to format a command according to the protocol. */
int mongovFormatCommand(char **target, const char *format, va_list ap);
int mongoFormatCommand(char **target, const char *format, ...);
int mongoFormatCommandArgv(char **target, int argc, const char **argv, const size_t *argvlen);
int mongoFormatSdsCommandArgv(sds *target, int argc, const char ** argv, const size_t *argvlen);
void mongoFreeCommand(char *cmd);
void mongoFreeSdsCommand(sds cmd);

enum mongoConnectionType {
    MONGO_CONN_TCP,
    MONGO_CONN_UNIX
};

/* Context for a connection to Mongo */
typedef struct mongoContext {
    int err; /* Error flags, 0 when there is no error */
    char errstr[128]; /* String representation of error when applicable */
    int fd;
    int flags;
    char *obuf; /* Write buffer */
    mongoReader *reader; /* Protocol reader */

    enum mongoConnectionType connection_type;
    struct timeval *timeout;

    struct {
        char *host;
        char *source_addr;
        int port;
    } tcp;

    struct {
        char *path;
    } unix_sock;

} mongoContext;

mongoContext *mongoConnect(const char *ip, int port);
mongoContext *mongoConnectWithTimeout(const char *ip, int port, const struct timeval tv);
mongoContext *mongoConnectNonBlock(const char *ip, int port);
mongoContext *mongoConnectBindNonBlock(const char *ip, int port,
                                       const char *source_addr);
mongoContext *mongoConnectBindNonBlockWithReuse(const char *ip, int port,
                                                const char *source_addr);
mongoContext *mongoConnectUnix(const char *path);
mongoContext *mongoConnectUnixWithTimeout(const char *path, const struct timeval tv);
mongoContext *mongoConnectUnixNonBlock(const char *path);
mongoContext *mongoConnectFd(int fd);

/**
 * Reconnect the given context using the saved information.
 *
 * This re-uses the exact same connect options as in the initial connection.
 * host, ip (or path), timeout and bind address are reused,
 * flags are used unmodified from the existing context.
 *
 * Returns MONGO_OK on successful connect or MONGO_ERR otherwise.
 */
int mongoReconnect(mongoContext *c);

int mongoSetTimeout(mongoContext *c, const struct timeval tv);
int mongoEnableKeepAlive(mongoContext *c);
void mongoFree(mongoContext *c);
int mongoFreeKeepFd(mongoContext *c);
int mongoBufferRead(mongoContext *c);
int mongoBufferWrite(mongoContext *c, int *done);

/* In a blocking context, this function first checks if there are unconsumed
 * replies to return and returns one if so. Otherwise, it flushes the output
 * buffer to the socket and reads until it has a reply. In a non-blocking
 * context, it will return unconsumed replies until there are no more. */
int mongoGetReply(mongoContext *c, void **reply);
int mongoGetReplyFromReader(mongoContext *c, void **reply);

/* Write a formatted command to the output buffer. Use these functions in blocking mode
 * to get a pipeline of commands. */
int mongoAppendFormattedCommand(mongoContext *c, const char *cmd, size_t len);

/* Write a command to the output buffer. Use these functions in blocking mode
 * to get a pipeline of commands. */
int mongovAppendCommand(mongoContext *c, const char *format, va_list ap);
int mongoAppendCommand(mongoContext *c, const char *format, ...);
int mongoAppendCommandArgv(mongoContext *c, int argc, const char **argv, const size_t *argvlen);

/* Issue a command to Mongo. In a blocking context, it is identical to calling
 * mongoAppendCommand, followed by mongoGetReply. The function will return
 * NULL if there was an error in performing the request, otherwise it will
 * return the reply. In a non-blocking context, it is identical to calling
 * only mongoAppendCommand and will always return NULL. */
void *mongovCommand(mongoContext *c, const char *format, va_list ap);
void *mongoCommand(mongoContext *c, const char *format, ...);
void *mongoCommandArgv(mongoContext *c, int argc, const char **argv, const size_t *argvlen);

#ifdef __cplusplus
}
#endif

#endif
