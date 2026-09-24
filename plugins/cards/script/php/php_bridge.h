#pragma once
#include <stddef.h>
#include <stdint.h>

/* No PHP values, allocations, exceptions or longjmps cross this C boundary. */
enum PhpOperation {
    PHP_LOAD, PHP_RESET, PHP_READ, PHP_WRITE, PHP_TIMER, PHP_SIGNAL,
    PHP_SAVE, PHP_RESTORE, PHP_RELEASE, PHP_STOP, PHP_SHUTDOWN
};
enum PhpHostOperation {
    PHP_HOST_LOG, PHP_HOST_READ, PHP_HOST_WRITE, PHP_HOST_TIME,
    PHP_HOST_SIGNAL_READ, PHP_HOST_SIGNAL_DRIVE, PHP_HOST_AFTER,
    PHP_HOST_ON_SIGNAL, PHP_HOST_PROJECT_READ, PHP_HOST_PROJECT_WRITE
};
struct PhpMessage {
    int operation;
    const char *text;
    size_t text_size;
    const char *data;
    size_t data_size;
    int64_t a, b, c;
};
struct PhpReply {
    int64_t value;
    const char *data; /* borrowed until the next call */
    size_t size;
    char error[1024];
};
typedef int (*PhpHostCall)(void *, const struct PhpMessage *, struct PhpReply *);
typedef int (*PhpRun)(const struct PhpMessage *, struct PhpReply *, PhpHostCall, void *);

#define SCRIPT_PHP_BRIDGE_ABI 2u
struct PhpBridgeApi {
    uint32_t abi_version;
    uint32_t struct_size;
    PhpRun run;
};
typedef const struct PhpBridgeApi *(*PhpGetApi)(void);
