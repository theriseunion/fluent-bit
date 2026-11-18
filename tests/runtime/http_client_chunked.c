/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <fluent-bit/flb_compat.h>

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_error.h>
#include <fluent-bit/flb_socket.h>
#include <fluent-bit/flb_upstream.h>
#include <fluent-bit/flb_engine.h>
#include <fluent-bit/flb_coro.h>
#include <fluent-bit/flb_http_client.h>

#include <monkey/mk_core.h>

#include "flb_tests_runtime.h"

struct runtime_http_client_ctx {
    struct flb_upstream   *u;
    struct flb_connection *u_conn;
    struct flb_config     *config;
    struct mk_event_loop  *evl;
};

struct chunked_server_ctx {
    int       listen_fd;
    int       port;
    pthread_t thread;
    const char **responses;
};

static int socket_write_all(int fd, const char *buffer, size_t length)
{
    ssize_t bytes;
    size_t  offset;

    offset = 0;

    while (offset < length) {
        bytes = send(fd, buffer + offset, length - offset, 0);
        if (bytes == -1) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (bytes == 0) {
            return -1;
        }

        offset += bytes;
    }

    return 0;
}

static int create_listen_socket(int *out_port)
{
    int fd;
    socklen_t length;
    struct sockaddr_in address;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);

    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) == -1) {
        flb_socket_close(fd);
        return -1;
    }

    if (listen(fd, 4) == -1) {
        flb_socket_close(fd);
        return -1;
    }

    length = sizeof(address);
    if (getsockname(fd, (struct sockaddr *) &address, &length) == -1) {
        flb_socket_close(fd);
        return -1;
    }

    *out_port = ntohs(address.sin_port);

    return fd;
}

static void *chunked_server_thread(void *data)
{
    int conn_fd;
    ssize_t bytes;
    char request[2048];
    size_t fragment_length;
    struct chunked_server_ctx *ctx;
    const char *fragments[] = {
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Trailer: Expires, X-Trace\r\n"
        "Connection: close\r\n"
        "\r\n"
        "4;foo=bar\r\n"
        "Wi",
        "ki\r\n"
        "5\r\n"
        "pedia\r\n",
        "0;done=yes\r\n"
        "Expires: tomorrow\r\n"
        "X-Trace: abc\r\n"
        "\r\n",
        NULL
    };
    int index;
    const char **selected;

    ctx = data;
    selected = ctx->responses ? ctx->responses : fragments;

    conn_fd = accept(ctx->listen_fd, NULL, NULL);
    if (conn_fd == -1) {
        return NULL;
    }

    bytes = recv(conn_fd, request, sizeof(request), 0);
    (void) bytes;

    for (index = 0; selected[index] != NULL; index++) {
        fragment_length = strlen(selected[index]);

        if (socket_write_all(conn_fd, selected[index], fragment_length) != 0) {
            break;
        }

        usleep(10000);
    }

    flb_socket_close(conn_fd);

    return NULL;
}

static struct runtime_http_client_ctx *runtime_http_client_ctx_create(int port, int synchronous)
{
    struct runtime_http_client_ctx *ctx;
    static int coro_initialized = FLB_FALSE;

    ctx = flb_calloc(1, sizeof(struct runtime_http_client_ctx));
    if (!TEST_CHECK(ctx != NULL)) {
        flb_errno();
        return NULL;
    }

    ctx->evl = mk_event_loop_create(16);
    if (!TEST_CHECK(ctx->evl != NULL)) {
        flb_free(ctx);
        return NULL;
    }

    flb_engine_evl_init();
    flb_engine_evl_set(ctx->evl);

    if (coro_initialized == FLB_FALSE) {
        flb_coro_init();
        coro_initialized = FLB_TRUE;
    }
    flb_coro_thread_init();

    ctx->config = flb_config_init();
    if (!TEST_CHECK(ctx->config != NULL)) {
        mk_event_loop_destroy(ctx->evl);
        flb_free(ctx);
        return NULL;
    }

    ctx->u = flb_upstream_create(ctx->config, "127.0.0.1", port, 0, NULL);
    if (!TEST_CHECK(ctx->u != NULL)) {
        flb_config_exit(ctx->config);
        mk_event_loop_destroy(ctx->evl);
        flb_free(ctx);
        return NULL;
    }

    if (synchronous) {
        flb_stream_disable_async_mode(&ctx->u->base);
    }
    ctx->u_conn = flb_upstream_conn_get(ctx->u);
    if (!TEST_CHECK(ctx->u_conn != NULL)) {
        flb_upstream_destroy(ctx->u);
        flb_config_exit(ctx->config);
        mk_event_loop_destroy(ctx->evl);
        flb_free(ctx);
        return NULL;
    }

    ctx->u_conn->upstream = ctx->u;

    return ctx;
}

static void runtime_http_client_ctx_destroy(struct runtime_http_client_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->u != NULL) {
        flb_upstream_destroy(ctx->u);
    }

    if (ctx->config != NULL) {
        flb_config_exit(ctx->config);
    }

    if (ctx->evl != NULL) {
        mk_event_loop_destroy(ctx->evl);
    }

    flb_free(ctx);
}

void test_http_client_chunked_runtime()
{
    int ret;
    int thread_started;
    size_t bytes_sent;
    flb_sds_t value;
    struct flb_http_client *client;
    struct chunked_server_ctx server;
    struct runtime_http_client_ctx *ctx;
    int payload_ready;
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(0x0201, &wsa_data);
#endif

    memset(&server, 0, sizeof(server));
    server.listen_fd = -1;
    client = NULL;
    ctx = NULL;
    value = NULL;
    payload_ready = FLB_FALSE;
    thread_started = FLB_FALSE;

    server.listen_fd = create_listen_socket(&server.port);
    TEST_CHECK(server.listen_fd != -1);
    if (server.listen_fd == -1) {
        return;
    }

    ret = pthread_create(&server.thread, NULL, chunked_server_thread, &server);
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        flb_socket_close(server.listen_fd);
        return;
    }
    thread_started = FLB_TRUE;

    ctx = runtime_http_client_ctx_create(server.port, 0);
    if (!TEST_CHECK(ctx != NULL)) {
        flb_socket_close(server.listen_fd);
        pthread_join(server.thread, NULL);
        return;
    }

    client = flb_http_client(ctx->u_conn, FLB_HTTP_GET, "/", NULL, 0,
                             "127.0.0.1", server.port, NULL, FLB_HTTP_11);
    TEST_CHECK(client != NULL);
    if (client == NULL) {
        goto cleanup;
    }

    ret = flb_http_do(client, &bytes_sent);
    if (!TEST_CHECK(ret == 0)) {
        goto cleanup;
    }

    if (!TEST_CHECK(client->resp.status == 200)) {
        goto cleanup;
    }

    payload_ready = TEST_CHECK(client->resp.payload != NULL);
    payload_ready &= TEST_CHECK(client->resp.payload_size == strlen("Wikipedia"));

    if (payload_ready) {
        TEST_CHECK(strncmp(client->resp.payload, "Wikipedia",
                           strlen("Wikipedia")) == 0);
    }
    else {
        goto cleanup;
    }

    value = flb_http_get_response_header(client, "X-Trace", 7);
    TEST_CHECK(value != NULL);
    if (value != NULL) {
        TEST_CHECK(strcmp(value, "abc") == 0);
        flb_sds_destroy(value);
    }

    value = flb_http_get_response_header(client, "Expires", 7);
    TEST_CHECK(value != NULL);
    if (value != NULL) {
        TEST_CHECK(strcmp(value, "tomorrow") == 0);
        flb_sds_destroy(value);
    }

cleanup:
    if (client != NULL) {
        flb_http_client_destroy(client);
    }

    if (ctx != NULL) {
        runtime_http_client_ctx_destroy(ctx);
    }
    if (server.listen_fd != -1) {
        flb_socket_close(server.listen_fd);
    }

    if (thread_started == FLB_TRUE) {
        pthread_join(server.thread, NULL);
    }
}


static void check_http_response(const char *response, int expected_success)
{
    struct chunked_server_ctx server;
    struct runtime_http_client_ctx *ctx;
    struct flb_http_client *client;
    const char *responses[] = {response, NULL};
    size_t bytes_sent;
    int ret;

    memset(&server, 0, sizeof(server));
    server.responses = responses;
    server.listen_fd = create_listen_socket(&server.port);
    if (!TEST_CHECK(server.listen_fd != -1)) {
        return;
    }
    ret = pthread_create(&server.thread, NULL, chunked_server_thread, &server);
    if (!TEST_CHECK(ret == 0)) {
        flb_socket_close(server.listen_fd);
        return;
    }
    ctx = runtime_http_client_ctx_create(server.port, 1);
    if (!TEST_CHECK(ctx != NULL)) {
        shutdown(server.listen_fd, SHUT_RDWR);
        flb_socket_close(server.listen_fd);
        pthread_join(server.thread, NULL);
        return;
    }
    client = flb_http_client(ctx->u_conn, FLB_HTTP_GET, "/", NULL, 0,
                             "127.0.0.1", server.port, NULL, FLB_HTTP_11);
    if (TEST_CHECK(client != NULL)) {
        ret = flb_http_do(client, &bytes_sent);
        if (expected_success) {
            TEST_CHECK(ret == 0);
            TEST_CHECK(client->resp.status == 200);
            TEST_CHECK(client->resp.payload_size == 4);
            if (client->resp.payload_size == 4) {
                TEST_CHECK(memcmp(client->resp.payload, "test", 4) == 0);
            }
        }
        else {
            TEST_CHECK(ret != 0);
        }
        flb_http_client_destroy(client);
    }
    runtime_http_client_ctx_destroy(ctx);
    flb_socket_close(server.listen_fd);
    pthread_join(server.thread, NULL);
}

void test_http_eof_empty(void)
{
    check_http_response("", 0);
}

void test_http_eof_header(void)
{
    check_http_response("HTTP/1.1 200 OK\r\nContent-Len", 0);
}

void test_http_eof_body(void)
{
    check_http_response("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nte", 0);
}

void test_http_eof_chunk(void)
{
    check_http_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nte", 0);
}

void test_http_complete_body(void)
{
    check_http_response("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ntest", 1);
}

TEST_LIST = {
    {"http_eof_empty", test_http_eof_empty},
    {"http_eof_header", test_http_eof_header},
    {"http_eof_body", test_http_eof_body},
    {"http_eof_chunk", test_http_eof_chunk},
    {"http_complete_body", test_http_complete_body},
    {"http_client_chunked_runtime", test_http_client_chunked_runtime},
    {0}
};
