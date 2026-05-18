# Standalone API documentation

This document describes using `libvalkey` in standalone (non-cluster) mode, including an overview of the synchronous and asynchronous APIs. It is not intended as a complete reference. For that it's always best to refer to the source code.

## Table of Contents

- [Synchronous API](#synchronous-api)
  - [Connecting](#connecting)
  - [Connection options](#connection-options)
  - [Executing commands](#executing-commands)
  - [Using replies](#using-replies)
  - [Reply types](#reply-types)
  - [Disconnecting/cleanup](#disconnecting-cleanup)
  - [Pipelining](#pipelining)
  - [Errors](#errors)
  - [Thread safety](#thread-safety)
  - [Reader configuration](#reader-configuration)
    - [Input buffer size](#maximum-input-buffer-size)
    - [Maximum array elements](#maximum-array-elements)
    - [RESP3 Push Replies](#resp3-push-replies)
    - [Allocator injection](#allocator-injection)
- [Asynchronous API](#asynchronous-api)
  - [Connecting](#connecting-1)
  - [Executing commands](#executing-commands-1)
  - [Disconnecting/cleanup](#disconnecting-cleanup-1)
- [TLS support](#tls-support)

## Synchronous API

The synchronous API has a pretty small surface area, with only a few commands to use. In general they are very similar to the way printf works, except they construct `RESP` commands.

### Connecting

There are several convenience functions to connect in various ways (e.g. host and port, Unix socket, etc). See [include/valkey/valkey.h](../include/valkey/valkey.h) for more details.

```c
valkeyContext *valkeyConnect(const char *host, int port);
valkeyContext *valkeyConnectUnix(const char *path);

// There is also a convenience struct to specify various options.
valkeyContext *valkeyConnectWithOptions(valkeyOptions *opt);
```

When connecting to a server, libvalkey will return `NULL` in the event that we can't allocate the context, and set the `err` member if we can connect but there are issues. So when connecting it's simple to handle error states.

```c
valkeyContext *ctx = valkeyConnect("localhost", 6379);
if (ctx == NULL || ctx->err) {
    fprintf(stderr, "Error connecting: %s\n", ctx ? ctx->errstr : "OOM");
}
```

### Connection options

There are a variety of options you can specify when connecting to the server, which are delivered via the `valkeyOptions` helper struct. This includes information to connect to the server as well as other flags.

```c
valkeyOptions opt = {0};

// You can set primary connection info
if (tcp)  {
    VALKEY_OPTIONS_SET_TCP(&opt, "localhost", 6379);
} else {
    VALKEY_OPTIONS_SET_UNIX(&opt, "/tmp/valkey.sock");
}

// You may attach any arbitrary data to the context
VALKEY_OPTIONS_SET_PRIVDATA(&opt, my_data);
```

There are also several flags you can specify when using the `valkeyOptions` helper struct.

| Flag | Description  |
| --- | --- |
| `VALKEY_OPT_NONBLOCK` | Tells libvalkey to make a non-blocking connection. |
| `VALKEY_OPT_REUSEADDR` | Tells libvalkey to set the [SO_REUSEADDR](https://man7.org/linux/man-pages/man7/socket.7.html) socket option |
| `VALKEY_OPT_PREFER_IPV4`<br>`VALKEY_OPT_PREFER_IPV6`<br>`VALKEY_OPT_PREFER_IP_UNSPEC` | Informs libvalkey to either prefer IPv4 or IPv6 when invoking [getaddrinfo](https://man7.org/linux/man-pages/man3/gai_strerror.3.html).  `VALKEY_OPT_PREFER_IP_UNSPEC` will cause libvalkey to specify `AF_UNSPEC` in the getaddrinfo call, which means both IPv4 and IPv6 addresses will be searched simultaneously.<br>Libvalkey prefers IPv4 by default. |
| `VALKEY_OPT_NO_PUSH_AUTOFREE` | Tells libvalkey to not install the default RESP3 PUSH handler (which just intercepts and frees the replies).  This is useful in situations where you want to process these messages in-band. |
| `VALKEY_OPT_NOAUTOFREEREPLIES` | **ASYNC**: tells libvalkey not to automatically invoke `freeReplyObject` after executing the reply callback. |
| `VALKEY_OPT_NOAUTOFREE` | **ASYNC**: Tells libvalkey not to automatically free the `valkeyAsyncContext` on connection/communication failure, but only if the user makes an explicit call to `valkeyAsyncDisconnect` or `valkeyAsyncFree` |
| `VALKEY_OPT_MPTCP` | Tells libvalkey to use multipath TCP (MPTCP). Note that only when both the server and client are using MPTCP do they establish an MPTCP connection between them; otherwise, they use a regular TCP connection instead. |

### Executing commands

The primary command interface is a `printf`-like function that takes a format string along with a variable number of arguments. This will construct a `RESP` command and deliver it to the server.

```c
valkeyReply *reply = valkeyCommand(ctx, "INCRBY %s %d", "counter", 42);

if (reply == NULL) {
    fprintf(stderr, "Communication error: %s\n", c->err ? c->errstr : "Unknown error");
} else if (reply->type == VALKEY_REPLY_ERROR) {
    fprintf(stderr, "Error response from server: %s\n", reply->str);
} else if (reply->type != VALKEY_REPLY_INTEGER) {
    // Very unlikely but should be checked.
    fprintf(stderr, "Error:  Non-integer reply to INCRBY?\n");
}

printf("New value of 'counter' is %lld\n", reply->integer);
freeReplyObject(reply);
```

If you need to deliver binary safe strings to the server, you can use the `%b` format specifier which requires you to pass the length as well.

```c
struct binary { int x; int y; } = {0xdeadbeef, 0xcafebabe};
valkeyReply *reply = valkeyCommand(ctx, "SET %s %b", "some-key", &binary, sizeof(binary));
```

Commands may also be constructed by sending an array of arguments along with an optional array of their lengths. If lengths are not provided, libvalkey will execute `strlen` on each argument.

```c
const char *argv[] = {"SET", "captain", "James Kirk"};
sonst size_t argvlens[] = {3, 7, 10};

valkeyReply *reply = valkeyCommandArgv(ctx, 3, argv, argvlens);
// Handle error conditions similarly to `valkeyCommand`
```

### Using replies

The `valkeyCommand` and `valkeyCommandArgv` functions return a `valkeyReply` on success and `NULL` in the event of a severe error (e.g. a communication failure with the server, out of memory condition, etc).

If the reply is `NULL` you can inspect the nature of the error by querying `valkeyContext->err` for the error code and `valkeyContext->errstr` for a human readable error string.

When a `valkeyReply` is returned, you should test the `valkeyReply->type` field to determine which kind of reply was received from the server. If for example there was an error in the command, this reply can be `VALKEY_REPLY_ERROR` and the specific error string will be in the `reply->str` member.

### Reply types

- `VALKEY_REPLY_ERROR` - An error reply. The error string is in `reply->str`.
- `VALKEY_REPLY_STATUS` - A status reply which will be in `reply->str`.
- `VALKEY_REPLY_INTEGER` - An integer reply, which will be in `reply->integer`.
- `VALKEY_REPLY_DOUBLE` - A double reply which will be in `reply->dval` as well as `reply->str`.
- `VALKEY_REPLY_NIL` - a nil reply.
- `VALKEY_REPLY_BOOL` - A boolean reply which will be in `reply->integer`.
- `VALKEY_REPLY_BIGNUM` - As of yet unused, but the string would be in `reply->str`.
- `VALKEY_REPLY_STRING` - A string reply which will be in `reply->str`.
- `VALKEY_REPLY_VERB` - A verbatim string reply which will be in `reply->str` and who's type will be in `reply->vtype`.
- `VALKEY_REPLY_ARRAY` - An array reply where each element is in `reply->element` with the number of elements in `reply->elements`.
- `VALKEY_REPLY_MAP` - A map reply, which structurally looks just like `VALKEY_REPLY_ARRAY` only is meant to represent keys and values. As with an array reply you can access the elements with `reply->element` and `reply->elements`.
- `VALKEY_REPLY_SET` - Another array-like reply representing a set (e.g. a reply from `SMEMBERS`). Access via `reply->element` and `reply->elements`.
- `VALKEY_REPLY_ATTR` - An attribute reply. As of yet unused by valkey-server.
- `VALKEY_REPLY_PUSH` - An out of band push reply. This is also array-like in nature.

### Disconnecting/cleanup

When libvalkey returns non-null `valkeyReply` struts you are responsible for freeing them with `freeReplyObject`.  In order to disconnect and free the context simply call `valkeyFree`.

```c
valkeyReply *reply = valkeyCommand(ctx, "set %s %s", "foo", "bar");
// Error handling ...
freeReplyObject(reply);

// Disconnect and free context
valkeyFree(ctx);
```

### Pipelining

`valkeyCommand` and `valkeyCommandArgv` each make a round-trip to the server, by sending the command and then waiting for a reply. Alternatively commands may be pipelined with the `valkeyAppendCommand` and `valkeyAppendCommandArgv` functions.

When you use `valkeyAppendCommand` the command is simply appended to the output buffer of `valkeyContext` but not delivered to the server, until you attempt to read the first response, at which point the entire buffer will be delivered.

```c
// No data will be delivered to the server while these commands are being appended.
for (size_t i = 0; i < 100000; i++) {
    if (valkeyAppendCommand(c, "INCRBY key:%zu %zu", i, i) != VALKEY_OK) {
        fprintf(stderr, "Error appending command: %s\n", c->errstr);
        exit(1);
    }
}

// The entire output buffer will be delivered on the first call to `valkeyGetReply`.
for (size_t i = 0; i < 100000; i++) {
    if (valkeyGetReply(c, (void**)&reply) != VALKEY_OK) {
        fprintf(stderr, "Error reading reply %zu: %s\n", i, c->errstr);
        exit(1);
    } else if (reply->type != VALKEY_REPLY_INTEGER) {
        fprintf(stderr, "Error:  Non-integer reply to INCRBY?\n");
        exit(1);
    }

    printf("INCRBY key:%zu => %lld\n", i, reply->integer);
    freeReplyObject(reply);
}
```

`valkeyGetReply` can also be used in other contexts than pipeline, for example when you want to continuously block for commands for example in a subscribe context.

```c
valkeyReply *reply = valkeyCommand(c, "SUBSCRIBE channel");
assert(reply != NULL && !c->err);

while (valkeyGetReply(c, (void**)&reply) == VALKEY_OK) {
    // Do something with the message...
    freeReplyObject(reply);
}
```

### Errors

As previously mentioned, when there is a communication error libvalkey will return `NULL` and set the `err` and `errstr` members with the nature of the problem. The specific error types are as follows.

- `VALKEY_ERR_IO` - A problem with the connection.
- `VALKEY_ERR_EOF` - The server closed the connection.
- `VALKEY_ERR_PROTOCOL` - There was an error parsing the reply.
- `VALKEY_ERR_TIMEOUT` - A connect, read, or write timeout.
- `VALKEY_ERR_OOM` - Out of memory.
- `VALKEY_ERR_OTHER` - Some other error (check `c->errstr` for details).

### Thread safety

Libvalkey context structs are **not** thread safe. You should not attempt to share them between threads, unless you really know what you're doing.

### Reader configuration

Libvalkey contexts have a few more mechanisms you can customize to your needs.

#### Maximum input buffer size

Libvalkey uses a buffer to hold incoming bytes, which is typically restored to the configurable max buffer size (`16KB`) when it is empty. To avoid continually reallocating this buffer you can set the value higher, or to zero which means "no limit".

```c
context->reader->maxbuf = 0;
```

#### Maximum array elements

By default, libvalkey will refuse to parse array-like replies if they have more than 2^32-1 or 4,294,967,295 elements. This value can be set to any arbitrary 64-bit value or zero which just means "no limit".

```c
context->reader->maxelements = 0;
```

#### RESP3 Push Replies

The `RESP` protocol introduced out-of-band "push" replies in the third version of the specification. These replies may come at any point in the data stream. By default, libvalkey will simply process these messages and discard them.

If your application needs to perform specific actions on PUSH messages you can install your own handler which will be called as they are received. It is also possible to set the push handler to NULL, in which case the messages will be delivered "in-band". This can be useful for example in a blocking subscribe loop.

**NOTE**: You may also specify a push handler in the `valkeyOptions` struct and set it on initialization .

#### Synchronous context

```c
void my_push_handler(void *privdata, void *reply) {
    // In a synchronous context, you are expected to free the reply after you're done with it.
}

// Initialization, etc.
valkeySetPushCallback(c, my_push_handler);
```

#### Asynchronous context

```c
void my_async_push_handler(valkeyAsyncContext *ac, void *reply) {
    // As with other async replies, libvalkey will free it for you, unless you have
    // configured the context with `VALKEY_OPT_NOAUTOFREE`.
}

// Initialization, etc
valkeyAsyncSetPushCallback(ac, my_async_push_handler);
```

#### Allocator injection

Internally libvalkey uses a layer of indirection from the standard allocation functions, by keeping a global structure with function pointers to the allocators we are going to use. By default they are just set to `malloc`, `calloc`, `realloc`, etc.

These can be overridden like so

```c
valkeyAllocFuncs my_allocators = {
    .mallocFn = my_malloc,
    .callocFn = my_calloc,
    .reallocFn = my_realloc,
    .strdupFn = my_strdup,
    .freeFn = my_free,
};

// libvalkey will return the previously set allocators.
valkeyAllocFuncs old = valkeySetAllocators(&my_allocators);
```

They can also be reset to the glibc or musl defaults

```c
valkeyResetAllocators();
```

**NOTE**: The `vk_calloc` function handles the case where `nmemb` * `size` would overflow a `size_t` and returns `NULL` in that case.

## Asynchronous API

Libvalkey also has an asynchronous API which supports a great many different event libraries. See the [examples](../examples) directory for specific information about each individual event library.

### Connecting

Libvalkey provides an `valkeyAsyncContext` to manage asynchronous connections which works similarly to the synchronous context.

```c
valkeyAsyncContext *ac = valkeyAsyncConnect("localhost", 6379);
if (ac == NULL) {
    fprintf(stderr, "Error:  Out of memory trying to allocate valkeyAsyncContext\n");
    exit(1);
} else if (ac->err) {
    fprintf(stderr, "Error: %s (%d)\n", ac->errstr, ac->err);
    exit(1);
}

// If we're using libev
valkeyLibevAttach(EV_DEFAULT_ ac);

valkeySetConnectCallback(ac, my_connect_callback);
valkeySetDisconnectCallback(ac, my_disconnect_callback);

ev_run(EV_DEFAULT_ 0);
```

The asynchronous context _should_ hold a connect callback function that is called when the connection attempt completes, either successfully or with an error.

It _can_ also hold a disconnect callback function that is called when the connection is disconnected (either because of an error or per user request).
The context object is always freed after the disconnect callback fired.

### Executing commands

Executing commands in an asynchronous context work similarly to the synchronous context, except that you can pass a callback that will be invoked when the reply is received.

```c
struct my_app_data {
    size_t incrby_replies;
    size_t get_replies;
};

void my_incrby_callback(valkeyAsyncContext *ac, void *r, void *privdata) {
    struct my_app_data *data = privdata;
    valkeyReply *reply = r;

    assert(reply != NULL && reply->type == VALKEY_REPLY_INTEGER);

    printf("Incremented value: %lld\n", reply->integer);
    data->incrby_replies++;
}

void my_get_callback(valkeyAsyncContext *ac, void *r, void *privdata) {
    struct my_app_data *data = privdata;
    valkeyReply *reply = r;

    assert(reply != NULL && reply->type == VALKEY_REPLY_STRING);

    printf("Key value: %s\n", reply->str);
    data->get_replies++;
}

int exec_some_commands(struct my_app_data *data) {
    valkeyAsyncCommand(ac, my_incrby_callback, data, "INCRBY mykey %d", 42);
    valkeyAsyncCommand(ac, my_get_callback, data, "GET %s", "mykey");
}
```

### Disconnecting/cleanup

For a graceful disconnect use `valkeyAsyncDisconnect` which will block new commands from being issued.
The connection is only terminated when all pending commands have been sent, their respective replies have been read, and their respective callbacks have been executed.
After this, the disconnection callback is called with the status, and the context object is freed.

To terminate the connection forcefully use `valkeyAsyncFree` which also will block new commands from being issued.
There will be no more data sent on the socket and all pending callbacks will be called with a `NULL` reply.
After this, the disconnection callback is called with the `VALKEY_OK` status, and the context object is freed.

## TLS support

TLS support is not enabled by default and requires an explicit build flag as described in [`README.md`](../README.md#building).

Libvalkey implements TLS on top of its `valkeyContext` and `valkeyAsyncContext`, so you will need to establish a connection first and then initiate a TLS handshake.
See the [examples](../examples) directory for how to create the TLS context and initiate the handshake.
