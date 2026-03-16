/*
 * Copyright (c) 2019, Redis Ltd.
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

#define VALKEYMODULE_CORE_MODULE /* A module that's part of the server core, uses server.h too. */

#include "server.h"
#include "connhelpers.h"
#include "adlist.h"
#include "io_threads.h"
#include "bio.h"

#if defined(USE_OPENSSL) &&                    \
    ((USE_OPENSSL == 1 /* BUILD_YES */) ||     \
     ((USE_OPENSSL == 2 /* BUILD_MODULE */) && \
      (defined(BUILD_TLS_MODULE) && BUILD_TLS_MODULE == 2)))

#include <openssl/conf.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/decoder.h>
#endif
#include <sys/stat.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#define REDIS_TLS_PROTO_TLSv1 (1 << 0)
#define REDIS_TLS_PROTO_TLSv1_1 (1 << 1)
#define REDIS_TLS_PROTO_TLSv1_2 (1 << 2)
#define REDIS_TLS_PROTO_TLSv1_3 (1 << 3)

/* Use safe defaults */
#ifdef TLS1_3_VERSION
#define REDIS_TLS_PROTO_DEFAULT (REDIS_TLS_PROTO_TLSv1_2 | REDIS_TLS_PROTO_TLSv1_3)
#else
#define REDIS_TLS_PROTO_DEFAULT (REDIS_TLS_PROTO_TLSv1_2)
#endif

SSL_CTX *valkey_tls_ctx = NULL;
SSL_CTX *valkey_tls_client_ctx = NULL;

static int parseProtocolsConfig(const char *str) {
    int i, count = 0;
    int protocols = 0;

    if (!str) return REDIS_TLS_PROTO_DEFAULT;
    sds *tokens = sdssplitlen(str, strlen(str), " ", 1, &count);

    if (!tokens) {
        serverLog(LL_WARNING, "Invalid tls-protocols configuration string");
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (!strcasecmp(tokens[i], "tlsv1"))
            protocols |= REDIS_TLS_PROTO_TLSv1;
        else if (!strcasecmp(tokens[i], "tlsv1.1"))
            protocols |= REDIS_TLS_PROTO_TLSv1_1;
        else if (!strcasecmp(tokens[i], "tlsv1.2"))
            protocols |= REDIS_TLS_PROTO_TLSv1_2;
        else if (!strcasecmp(tokens[i], "tlsv1.3")) {
#ifdef TLS1_3_VERSION
            protocols |= REDIS_TLS_PROTO_TLSv1_3;
#else
            serverLog(LL_WARNING, "TLSv1.3 is specified in tls-protocols but not supported by OpenSSL.");
            protocols = -1;
            break;
#endif
        } else {
            serverLog(LL_WARNING, "Invalid tls-protocols specified. "
                                  "Use a combination of 'TLSv1', 'TLSv1.1', 'TLSv1.2' and 'TLSv1.3'.");
            protocols = -1;
            break;
        }
    }
    sdsfreesplitres(tokens, count);

    return protocols;
}

/* list of connections with pending data already read from the socket, but not
 * served to the reader yet. */
static list *pending_list = NULL;

/**
 * OpenSSL global initialization and locking handling callbacks.
 * Note that this is only required for OpenSSL < 1.1.0.
 */

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define USE_CRYPTO_LOCKS
#endif

#ifdef USE_CRYPTO_LOCKS

static pthread_mutex_t *openssl_locks;

static void sslLockingCallback(int mode, int lock_id, const char *f, int line) {
    pthread_mutex_t *mt = openssl_locks + lock_id;

    if (mode & CRYPTO_LOCK) {
        pthread_mutex_lock(mt);
    } else {
        pthread_mutex_unlock(mt);
    }

    (void)f;
    (void)line;
}

static void initCryptoLocks(void) {
    unsigned i, nlocks;
    if (CRYPTO_get_locking_callback() != NULL) {
        /* Someone already set the callback before us. Don't destroy it! */
        return;
    }
    nlocks = CRYPTO_num_locks();
    openssl_locks = zmalloc(sizeof(*openssl_locks) * nlocks);
    for (i = 0; i < nlocks; i++) {
        pthread_mutex_init(openssl_locks + i, NULL);
    }
    CRYPTO_set_locking_callback(sslLockingCallback);
}
#endif /* USE_CRYPTO_LOCKS */

static void tlsInit(void) {
/* Enable configuring OpenSSL using the standard openssl.cnf
 * OPENSSL_config()/OPENSSL_init_crypto() should be the first
 * call to the OpenSSL* library.
 *  - OPENSSL_config() should be used for OpenSSL versions < 1.1.0
 *  - OPENSSL_init_crypto() should be used for OpenSSL versions >= 1.1.0
 */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    OPENSSL_config(NULL);
    SSL_load_error_strings();
    SSL_library_init();
#elif OPENSSL_VERSION_NUMBER < 0x10101000L
    OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CONFIG, NULL);
#else
    OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CONFIG | OPENSSL_INIT_ATFORK, NULL);
#endif

#ifdef USE_CRYPTO_LOCKS
    initCryptoLocks();
#endif

    if (!RAND_poll()) {
        serverLog(LL_WARNING, "OpenSSL: Failed to seed random number generator.");
    }

    pending_list = listCreate();
}

static void tlsClearCertInfo(long long *expiry, sds *serial);
static void tlsClearCACertInfo(void);
static void tlsClearAllCertInfo(void);
static void tlsRefreshAllCertInfo(void);

static void tlsCleanup(void) {
    if (valkey_tls_ctx) {
        SSL_CTX_free(valkey_tls_ctx);
        valkey_tls_ctx = NULL;
    }
    if (valkey_tls_client_ctx) {
        SSL_CTX_free(valkey_tls_client_ctx);
        valkey_tls_client_ctx = NULL;
    }
    tlsClearAllCertInfo();

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
    // unavailable on LibreSSL
    OPENSSL_cleanup();
#endif
}

/* Convert ASN1_TIME into a UTC tm plus a timezone offset (seconds). */
static int tlsAsn1TimeToTm(const ASN1_TIME *time, struct tm *tm, int *tz_off) {
    if (!time || !tm) return 0;
#if OPENSSL_VERSION_NUMBER >= 0x10002000L || defined(LIBRESSL_VERSION_NUMBER)
    if (ASN1_TIME_to_tm(time, tm)) {
        if (tz_off) *tz_off = 0;
        return 1;
    }
#endif
    return 0;
}

/* Civil-from-fixed algorithm: convert Y/M/D to absolute days since Unix epoch. */
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* Convert a UTC tm to a unix timestamp (seconds since epoch). */
static long long tmToEpochUTC(const struct tm *tm) {
    int64_t year = tm->tm_year + 1900;
    unsigned month = tm->tm_mon + 1;
    unsigned day = tm->tm_mday;
    int64_t days = daysFromCivil(year, month, day);
    int64_t seconds = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
    return (long long)seconds;
}

/* Helper that returns the unix timestamp for an ASN1_TIME value. */
static int asn1TimeToEpoch(const ASN1_TIME *time, long long *epoch) {
    struct tm tm;
    int tz_offset = 0;
    if (!tlsAsn1TimeToTm(time, &tm, &tz_offset)) return 0;
    long long ts = tmToEpochUTC(&tm);
    ts -= tz_offset;
    if (epoch) *epoch = ts;
    return 1;
}

static int tlsGetX509Expiry(X509 *cert, long long *expiry) {
    if (!cert) return C_ERR;
    const ASN1_TIME *not_after = X509_get0_notAfter(cert);
    if (!not_after) return C_ERR;
    return asn1TimeToEpoch(not_after, expiry) ? C_OK : C_ERR;
}

void tlsResetCertInfo(void) {
    if (server.tls_port || server.tls_replication || server.tls_cluster) {
        tlsRefreshAllCertInfo();
        return;
    }
    tlsClearAllCertInfo();
}

/* Convert a certificate serial number to hex string for INFO reporting. */
static sds tlsX509SerialToSds(X509 *cert) {
    if (!cert) return NULL;
    ASN1_INTEGER *serial = X509_get_serialNumber(cert);
    if (!serial) return NULL;
    sds serial_sds = NULL;
    BIGNUM *bn = ASN1_INTEGER_to_BN(serial, NULL);
    if (bn) {
        char *hex = BN_bn2hex(bn);
        if (hex) {
            serial_sds = sdsnew(hex);
            OPENSSL_free(hex);
        }
        BN_free(bn);
    }
    return serial_sds;
}

static void tlsClearCertSerial(sds *serial) {
    if (*serial) {
        sdsfree(*serial);
        *serial = NULL;
    }
}

static int tlsStoreCertInfo(long long expiry, sds serial, int count, long long *out_expiry, sds *out_serial, int *out_count) {
    if (out_count) *out_count = count;
    tlsClearCertSerial(out_serial);
    if (expiry == 0) {
        if (serial) sdsfree(serial);
        if (out_count) *out_count = 0;
        return C_ERR;
    }
    if (out_expiry) *out_expiry = expiry;
    *out_serial = serial;
    return C_OK;
}

static int tlsUpdateCertInfoFromCtx(SSL_CTX *ctx, long long *expiry, sds *serial) {
    if (!ctx) return C_ERR;
    X509 *cert = SSL_CTX_get0_certificate(ctx);
    if (tlsGetX509Expiry(cert, expiry) != C_OK) return C_ERR;
    tlsClearCertSerial(serial);
    *serial = tlsX509SerialToSds(cert);
    return C_OK;
}

static int tlsUpdateCertInfoFromFileHandle(FILE *fp, long long *expiry, sds *serial, int *count) {
    int cert_count = 0;
    long long earliest_expiry = 0;
    sds earliest_serial = NULL;
    X509 *cert = NULL;
    while ((cert = PEM_read_X509(fp, NULL, NULL, NULL)) != NULL) {
        cert_count++;
        long long cert_expiry = 0;
        if (tlsGetX509Expiry(cert, &cert_expiry) == C_OK) {
            if (earliest_expiry == 0 || cert_expiry < earliest_expiry) {
                earliest_expiry = cert_expiry;
                if (earliest_serial) sdsfree(earliest_serial);
                earliest_serial = tlsX509SerialToSds(cert);
            }
        }
        X509_free(cert);
    }
    if (count) *count = cert_count;
    if (earliest_expiry == 0) {
        if (earliest_serial) sdsfree(earliest_serial);
        if (count) *count = 0;
        return C_ERR;
    }
    if (expiry) *expiry = earliest_expiry;
    *serial = earliest_serial;
    return C_OK;
}

static void tlsMergeCertInfo(long long *expiry, sds *serial, int *count, long long src_expiry, sds src_serial, int src_count) {
    if (count) *count += src_count;
    if (src_expiry > 0 && (*expiry == 0 || src_expiry < *expiry)) {
        if (*serial) sdsfree(*serial);
        *expiry = src_expiry;
        *serial = src_serial;
    } else if (src_serial) {
        sdsfree(src_serial);
    }
}

static int tlsUpdateCertInfoFromFile(const char *path, long long *expiry, sds *serial, int *count) {
    if (!path) return C_ERR;
    FILE *fp = fopen(path, "r");
    if (!fp) return C_ERR;
    long long file_expiry = 0;
    sds file_serial = NULL;
    int file_count = 0;
    int result = tlsUpdateCertInfoFromFileHandle(fp, &file_expiry, &file_serial, &file_count);
    fclose(fp);
    if (result == C_ERR) {
        return tlsStoreCertInfo(0, file_serial, file_count, expiry, serial, count);
    }
    return tlsStoreCertInfo(file_expiry, file_serial, file_count, expiry, serial, count);
}

static int tlsUpdateCertInfoFromDir(const char *path, long long *expiry, sds *serial, int *count) {
    if (!path) return C_ERR;
    DIR *dir = opendir(path);
    if (!dir) return C_ERR;
    int cert_count = 0;
    long long earliest_expiry = 0;
    sds earliest_serial = NULL;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char fullpath[PATH_MAX];
        if (snprintf(fullpath, sizeof(fullpath), "%s/%s", path, de->d_name) >= (int)sizeof(fullpath)) continue;
        struct stat st;
        if (stat(fullpath, &st) == -1) continue;
        if (!S_ISREG(st.st_mode)) continue;
        FILE *fp = fopen(fullpath, "r");
        if (!fp) continue;
        long long file_expiry = 0;
        sds file_serial = NULL;
        int file_count = 0;
        if (tlsUpdateCertInfoFromFileHandle(fp, &file_expiry, &file_serial, &file_count) == C_OK) {
            tlsMergeCertInfo(&earliest_expiry, &earliest_serial, &cert_count, file_expiry, file_serial, file_count);
        } else {
            if (file_serial) sdsfree(file_serial);
        }
        fclose(fp);
    }
    closedir(dir);
    return tlsStoreCertInfo(earliest_expiry, earliest_serial, cert_count, expiry, serial, count);
}

static void tlsRefreshServerCertInfo(void) {
    if (!(server.tls_port || server.tls_replication || server.tls_cluster) || !valkey_tls_ctx ||
        tlsUpdateCertInfoFromCtx(valkey_tls_ctx, &server.tls_server_cert_expire_time, &server.tls_server_cert_serial) == C_ERR) {
        tlsClearCertInfo(&server.tls_server_cert_expire_time, &server.tls_server_cert_serial);
    }
}

static void tlsRefreshClientCertInfo(void) {
    if (tlsUpdateCertInfoFromCtx(valkey_tls_client_ctx, &server.tls_client_cert_expire_time, &server.tls_client_cert_serial) == C_ERR) {
        tlsClearCertInfo(&server.tls_client_cert_expire_time, &server.tls_client_cert_serial);
    }
}

static void tlsRefreshCACertInfo(void) {
    long long file_expiry = 0, dir_expiry = 0;
    sds file_serial = NULL, dir_serial = NULL;
    int file_count = 0, dir_count = 0;
    int file_ok = tlsUpdateCertInfoFromFile(server.tls_ctx_config.ca_cert_file,
                                            &file_expiry,
                                            &file_serial,
                                            &file_count) == C_OK;
    int dir_ok = tlsUpdateCertInfoFromDir(server.tls_ctx_config.ca_cert_dir,
                                          &dir_expiry,
                                          &dir_serial,
                                          &dir_count) == C_OK;

    tlsClearCACertInfo();
    if (!file_ok && !dir_ok) {
        if (file_serial) sdsfree(file_serial);
        if (dir_serial) sdsfree(dir_serial);
        return;
    }

    if (file_ok && (!dir_ok || file_expiry <= dir_expiry)) {
        server.tls_ca_cert_expire_time = file_expiry;
        server.tls_ca_cert_serial = file_serial;
        if (dir_serial) sdsfree(dir_serial);
    } else {
        server.tls_ca_cert_expire_time = dir_expiry;
        server.tls_ca_cert_serial = dir_serial;
        if (file_serial) sdsfree(file_serial);
    }
}

static void tlsRefreshAllCertInfo(void) {
    tlsRefreshServerCertInfo();
    tlsRefreshClientCertInfo();
    tlsRefreshCACertInfo();
}

/* Callback for passing a keyfile password stored as an sds to OpenSSL */
static int tlsPasswordCallback(char *buf, int size, int rwflag, void *u) {
    UNUSED(rwflag);

    const char *pass = u;
    size_t pass_len;

    if (!pass) return -1;
    pass_len = strlen(pass);
    if (pass_len > (size_t)size) return -1;
    memcpy(buf, pass, pass_len);

    return (int)pass_len;
}

/* Check a single X509 certificate validity */
static bool isCertValid(X509 *cert) {
    if (!cert) return false;
    const ASN1_TIME *not_before = X509_get0_notBefore(cert);
    const ASN1_TIME *not_after = X509_get0_notAfter(cert);
    if (!not_before || !not_after) return false;
    if (X509_cmp_current_time(not_before) > 0 ||
        X509_cmp_current_time(not_after) < 0) {
        return false;
    }
    return true;
}

/* Load all certificates from a directory into the X509_STORE
 * Returns true on success, false on failure */
static bool loadCaCertDir(SSL_CTX *ctx, const char *ca_cert_dir) {
    if (!ca_cert_dir) return true;

    DIR *dir;
    struct dirent *entry;
    char full_path[PATH_MAX];
    X509_STORE *store = SSL_CTX_get_cert_store(ctx);

    if (!store) {
        serverLog(LL_WARNING, "Failed to get X509_STORE from SSL_CTX");
        return false;
    }

    dir = opendir(ca_cert_dir);
    if (!dir) {
        serverLog(LL_WARNING, "Failed to open CA certificate directory: %s", ca_cert_dir);
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        snprintf(full_path, sizeof(full_path), "%s/%s", ca_cert_dir, entry->d_name);
        FILE *fp = fopen(full_path, "r");
        if (!fp) continue;

        X509 *cert = PEM_read_X509(fp, NULL, NULL, NULL);
        fclose(fp);

        if (cert) {
            if (X509_STORE_add_cert(store, cert) != 1) {
                unsigned long err = ERR_peek_last_error();
                if (ERR_GET_REASON(err) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
                    serverLog(LL_WARNING, "Failed to add CA certificate from %s to store", full_path);
                    X509_free(cert);
                    closedir(dir);
                    return false;
                }
                ERR_clear_error();
            }
            X509_free(cert);
        }
    }

    closedir(dir);
    return true;
}

/* Iterate over all CA certs in the SSL_CTX and fail-fast if any are invalid */
static bool areAllCaCertsValid(SSL_CTX *ctx) {
    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    if (!store) return false;
    STACK_OF(X509_OBJECT) *objs = X509_STORE_get0_objects(store);
    if (!objs) return false;
    for (int i = 0; i < sk_X509_OBJECT_num(objs); i++) {
        X509_OBJECT *obj = sk_X509_OBJECT_value(objs, i);
        int type = X509_OBJECT_get_type(obj);
        if (type == X509_LU_X509) {
            X509 *ca_cert = X509_OBJECT_get0_X509(obj);
            if (ca_cert && !isCertValid(ca_cert)) {
                return false;
            }
        }
    }
    return true;
}

/* Create a *base* SSL_CTX using the SSL configuration provided. The base context
 * includes everything that's common for both client-side and server-side connections.
 */
static SSL_CTX *createSSLContext(serverTLSContextConfig *ctx_config, int protocols, int client) {
    const char *cert_file = client ? ctx_config->client_cert_file : ctx_config->cert_file;
    const char *key_file = client ? ctx_config->client_key_file : ctx_config->key_file;
    const char *key_file_pass = client ? ctx_config->client_key_file_pass : ctx_config->key_file_pass;
    char errbuf[256];
    SSL_CTX *ctx = NULL;

    ctx = SSL_CTX_new(SSLv23_method());
    if (!ctx) goto error;

    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
    SSL_CTX_set_options(ctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
#endif

    if (!(protocols & REDIS_TLS_PROTO_TLSv1)) SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1);
    if (!(protocols & REDIS_TLS_PROTO_TLSv1_1)) SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1_1);
#ifdef SSL_OP_NO_TLSv1_2
    if (!(protocols & REDIS_TLS_PROTO_TLSv1_2)) SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1_2);
#endif
#ifdef SSL_OP_NO_TLSv1_3
    if (!(protocols & REDIS_TLS_PROTO_TLSv1_3)) SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1_3);
#endif

#ifdef SSL_OP_NO_COMPRESSION
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
#endif

    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);

    SSL_CTX_set_default_passwd_cb(ctx, tlsPasswordCallback);
    SSL_CTX_set_default_passwd_cb_userdata(ctx, (void *)key_file_pass);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) <= 0) {
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        serverLog(LL_WARNING, "Failed to load certificate: %s: %s", cert_file, errbuf);
        goto error;
    }

    if (!isCertValid(SSL_CTX_get0_certificate(ctx))) {
        serverLog(LL_WARNING, "%s TLS certificate is invalid. Aborting TLS configuration.", client ? "Client" : "Server");
        goto error;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        serverLog(LL_WARNING, "Failed to load private key: %s: %s", key_file, errbuf);
        goto error;
    }

    if (ctx_config->ca_cert_file || ctx_config->ca_cert_dir) {
        if (SSL_CTX_load_verify_locations(ctx, ctx_config->ca_cert_file, ctx_config->ca_cert_dir) <= 0) {
            ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
            serverLog(LL_WARNING, "Failed to configure CA certificate(s) file/directory: %s", errbuf);
            goto error;
        }

        if (!loadCaCertDir(ctx, ctx_config->ca_cert_dir)) {
            serverLog(LL_WARNING, "Failed to load CA certificates from directory: %s", ctx_config->ca_cert_dir);
            goto error;
        }

        if (!areAllCaCertsValid(ctx)) {
            serverLog(LL_WARNING, "One or more loaded CA certificates are invalid. Aborting TLS configuration.");
            goto error;
        }
    }

    if (ctx_config->ciphers && !SSL_CTX_set_cipher_list(ctx, ctx_config->ciphers)) {
        serverLog(LL_WARNING, "Failed to configure ciphers: %s", ctx_config->ciphers);
        goto error;
    }

#ifdef TLS1_3_VERSION
    if (ctx_config->ciphersuites && !SSL_CTX_set_ciphersuites(ctx, ctx_config->ciphersuites)) {
        serverLog(LL_WARNING, "Failed to configure ciphersuites: %s", ctx_config->ciphersuites);
        goto error;
    }
#endif

    return ctx;

error:
    if (ctx) SSL_CTX_free(ctx);
    return NULL;
}

/* Helper function to create SSL contexts from config.
 * This does the CPU-intensive work of parsing certificates and creating SSL contexts.
 * Returns C_OK on success, C_ERR on failure.
 * On success, *ctx and *client_ctx are set (client_ctx may be NULL).
 * On failure, both are set to NULL. */
static int tlsCreateContexts(serverTLSContextConfig *ctx_config, SSL_CTX **out_ctx, SSL_CTX **out_client_ctx) {
    char errbuf[256];
    SSL_CTX *ctx = NULL;
    SSL_CTX *client_ctx = NULL;

    if (!ctx_config->cert_file) {
        serverLog(LL_WARNING, "No tls-cert-file configured!");
        goto error;
    }

    if (!ctx_config->key_file) {
        serverLog(LL_WARNING, "No tls-key-file configured!");
        goto error;
    }

    if (((server.tls_auth_clients != TLS_CLIENT_AUTH_NO) || server.tls_cluster || server.tls_replication) &&
        !ctx_config->ca_cert_file && !ctx_config->ca_cert_dir) {
        serverLog(LL_WARNING, "Either tls-ca-cert-file or tls-ca-cert-dir must be specified when tls-cluster, "
                              "tls-replication or tls-auth-clients are enabled!");
        goto error;
    }

    int protocols = parseProtocolsConfig(ctx_config->protocols);
    if (protocols == -1) goto error;

    /* Create server side/general context */
    ctx = createSSLContext(ctx_config, protocols, 0);
    if (!ctx) goto error;

    if (ctx_config->session_caching) {
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(ctx, ctx_config->session_cache_size);
        SSL_CTX_set_timeout(ctx, ctx_config->session_cache_timeout);
        SSL_CTX_set_session_id_context(ctx, (void *)"redis", 5);
    } else {
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    }

#ifdef SSL_OP_NO_CLIENT_RENEGOTIATION
    SSL_CTX_set_options(ctx, SSL_OP_NO_CLIENT_RENEGOTIATION);
#endif

    if (ctx_config->prefer_server_ciphers) SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);

#if ((OPENSSL_VERSION_NUMBER < 0x30000000L) && defined(SSL_CTX_set_ecdh_auto))
    SSL_CTX_set_ecdh_auto(ctx, 1);
#endif
    SSL_CTX_set_options(ctx, SSL_OP_SINGLE_DH_USE);

    if (ctx_config->dh_params_file) {
        FILE *dhfile = fopen(ctx_config->dh_params_file, "r");
        if (!dhfile) {
            serverLog(LL_WARNING, "Failed to load %s: %s", ctx_config->dh_params_file, strerror(errno));
            goto error;
        }

#if (OPENSSL_VERSION_NUMBER >= 0x30000000L)
        EVP_PKEY *pkey = NULL;
        OSSL_DECODER_CTX *dctx =
            OSSL_DECODER_CTX_new_for_pkey(&pkey, "PEM", NULL, "DH", OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS, NULL, NULL);
        if (!dctx) {
            serverLog(LL_WARNING, "No decoder for DH params.");
            fclose(dhfile);
            goto error;
        }
        if (!OSSL_DECODER_from_fp(dctx, dhfile)) {
            serverLog(LL_WARNING, "%s: failed to read DH params.", ctx_config->dh_params_file);
            OSSL_DECODER_CTX_free(dctx);
            fclose(dhfile);
            goto error;
        }

        OSSL_DECODER_CTX_free(dctx);
        fclose(dhfile);

        if (SSL_CTX_set0_tmp_dh_pkey(ctx, pkey) <= 0) {
            ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
            serverLog(LL_WARNING, "Failed to load DH params file: %s: %s", ctx_config->dh_params_file, errbuf);
            EVP_PKEY_free(pkey);
            goto error;
        }
        /* Not freeing pkey, it is owned by OpenSSL now */
#else
        DH *dh = PEM_read_DHparams(dhfile, NULL, NULL, NULL);
        fclose(dhfile);
        if (!dh) {
            serverLog(LL_WARNING, "%s: failed to read DH params.", ctx_config->dh_params_file);
            goto error;
        }

        if (SSL_CTX_set_tmp_dh(ctx, dh) <= 0) {
            ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
            serverLog(LL_WARNING, "Failed to load DH params file: %s: %s", ctx_config->dh_params_file, errbuf);
            DH_free(dh);
            goto error;
        }

        DH_free(dh);
#endif
    } else {
#if (OPENSSL_VERSION_NUMBER >= 0x30000000L)
        SSL_CTX_set_dh_auto(ctx, 1);
#endif
    }

    /* If a client-side certificate is configured, create an explicit client context */
    if (ctx_config->client_cert_file && ctx_config->client_key_file) {
        client_ctx = createSSLContext(ctx_config, protocols, 1);
        if (!client_ctx) goto error;
    }

    *out_ctx = ctx;
    *out_client_ctx = client_ctx;
    return C_OK;

error:
    if (ctx) SSL_CTX_free(ctx);
    if (client_ctx) SSL_CTX_free(client_ctx);
    *out_ctx = NULL;
    *out_client_ctx = NULL;
    return C_ERR;
}

/* TLS materials metadata for change detection */
typedef struct {
    unsigned char cert_fingerprint[EVP_MAX_MD_SIZE];
    unsigned int cert_fingerprint_len;
    unsigned char client_cert_fingerprint[EVP_MAX_MD_SIZE];
    unsigned int client_cert_fingerprint_len;
    unsigned char ca_cert_fingerprint[EVP_MAX_MD_SIZE];
    unsigned int ca_cert_fingerprint_len;
    ino_t ca_cert_dir_inode;
    time_t ca_cert_dir_mtime;
    ino_t key_file_inode;
    time_t key_file_mtime;
    ino_t client_key_file_inode;
    time_t client_key_file_mtime;
} tlsMaterialsMetadata;

/* Pending TLS reload that holds both SSL contexts and their metadata.
 * Updated serially by BIO thread, applied atomically by main thread. */
typedef struct {
    SSL_CTX *ctx;
    SSL_CTX *client_ctx;
    tlsMaterialsMetadata metadata;
} tlsPendingReload;

/* Last known (active) TLS materials metadata */
static tlsMaterialsMetadata active_metadata = {0};

/* Compute certificate fingerprint from file. */
static int getCertFingerprint(const char *cert_file, unsigned char *fingerprint, unsigned int *fingerprint_len) {
    if (!cert_file) return C_ERR;

    FILE *fp = fopen(cert_file, "r");
    if (!fp) {
        serverLog(LL_WARNING, "Failed to open certificate file '%s': %s", cert_file, strerror(errno));
        return C_ERR;
    }

    X509 *cert = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);
    if (!cert) {
        serverLog(LL_WARNING, "Failed to parse X509 certificate from '%s'", cert_file);
        return C_ERR;
    }

    const EVP_MD *digest = EVP_sha256();
    if (X509_digest(cert, digest, fingerprint, fingerprint_len) != 1) {
        serverLog(LL_WARNING, "Failed to compute certificate fingerprint for '%s'", cert_file);
        X509_free(cert);
        return C_ERR;
    }

    X509_free(cert);
    return C_OK;
}

/* Capture current metadata from files into a metadata structure. */
static void captureMetadata(serverTLSContextConfig *ctx_config, tlsMaterialsMetadata *metadata) {
    memset(metadata, 0, sizeof(*metadata));

    /* Certificate files: fingerprint-based detection */
    getCertFingerprint(ctx_config->cert_file, metadata->cert_fingerprint, &metadata->cert_fingerprint_len);
    getCertFingerprint(ctx_config->client_cert_file, metadata->client_cert_fingerprint, &metadata->client_cert_fingerprint_len);
    getCertFingerprint(ctx_config->ca_cert_file, metadata->ca_cert_fingerprint, &metadata->ca_cert_fingerprint_len);

    /* Key files and CA dir: inode + mtime */
    struct stat st;
    if (ctx_config->ca_cert_dir && stat(ctx_config->ca_cert_dir, &st) == 0) {
        metadata->ca_cert_dir_inode = st.st_ino;
        metadata->ca_cert_dir_mtime = st.st_mtime;
    }
    if (ctx_config->key_file && stat(ctx_config->key_file, &st) == 0) {
        metadata->key_file_inode = st.st_ino;
        metadata->key_file_mtime = st.st_mtime;
    }
    if (ctx_config->client_key_file && stat(ctx_config->client_key_file, &st) == 0) {
        metadata->client_key_file_inode = st.st_ino;
        metadata->client_key_file_mtime = st.st_mtime;
    }
}

/* Compare two metadata structures to detect changes. */
static int metadataChanged(const tlsMaterialsMetadata *old, const tlsMaterialsMetadata *new) {
    /* Check certificate fingerprints */
    if (old->cert_fingerprint_len != new->cert_fingerprint_len ||
        (new->cert_fingerprint_len > 0 && memcmp(old->cert_fingerprint, new->cert_fingerprint, new->cert_fingerprint_len) != 0)) {
        return 1;
    }

    if (old->client_cert_fingerprint_len != new->client_cert_fingerprint_len ||
        (new->client_cert_fingerprint_len > 0 && memcmp(old->client_cert_fingerprint, new->client_cert_fingerprint, new->client_cert_fingerprint_len) != 0)) {
        return 1;
    }

    if (old->ca_cert_fingerprint_len != new->ca_cert_fingerprint_len ||
        (new->ca_cert_fingerprint_len > 0 && memcmp(old->ca_cert_fingerprint, new->ca_cert_fingerprint, new->ca_cert_fingerprint_len) != 0)) {
        return 1;
    }

    /* Check inode/mtime */
    if (old->ca_cert_dir_inode != new->ca_cert_dir_inode || old->ca_cert_dir_mtime != new->ca_cert_dir_mtime) {
        return 1;
    }
    if (old->key_file_inode != new->key_file_inode || old->key_file_mtime != new->key_file_mtime) {
        return 1;
    }
    if (old->client_key_file_inode != new->client_key_file_inode || old->client_key_file_mtime != new->client_key_file_mtime) {
        return 1;
    }

    return 0;
}


/* TLS background reload state */
static _Atomic long long lastTlsConfigureTime = 0;
static tlsPendingReload pending_reload = {0};
static pthread_mutex_t pending_reload_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Attempt to configure/reconfigure TLS. This operation is atomic and will
 * leave the SSL_CTX unchanged if it fails.
 *
 * If reconfigure is true, always reconfigure; if false, only configure if
 * valkey_tls_ctx is NULL.
 *
 * If background is true, check for changes and do heavy work in background thread;
 * if false, do work synchronously and swap immediately.
 */
static int tlsConfigure(void *priv, int reconfigure, bool background) {
    serverTLSContextConfig *ctx_config = (serverTLSContextConfig *)priv;
    SSL_CTX *ctx = NULL;
    SSL_CTX *client_ctx = NULL;

    if (!reconfigure && valkey_tls_ctx) {
        return C_OK;
    }

    if (reconfigure) {
        serverLog(LL_DEBUG, background ? "Background TLS reconfiguration started" : "Reconfiguring TLS");
    } else {
        serverLog(LL_DEBUG, "Configuring TLS");
    }

    if (background && reconfigure) {
        tlsMaterialsMetadata new_metadata;
        captureMetadata(ctx_config, &new_metadata);

        if (!metadataChanged(&active_metadata, &new_metadata)) {
            serverLog(LL_DEBUG, "TLS reload skipped: materials unchanged");
            atomic_store_explicit(&lastTlsConfigureTime, server.ustime, memory_order_relaxed);
            return C_OK;
        }
        serverLog(LL_NOTICE, "TLS materials changed, reloading in background");

        if (tlsCreateContexts(ctx_config, &ctx, &client_ctx) == C_ERR) {
            serverLog(LL_WARNING, "Background TLS reload failed");
            return C_ERR;
        }

        pthread_mutex_lock(&pending_reload_mutex);
        if (pending_reload.ctx) {
            SSL_CTX_free(pending_reload.ctx);
            SSL_CTX_free(pending_reload.client_ctx);
            serverLog(LL_DEBUG, "Replacing previous pending TLS reload");
        }
        pending_reload.ctx = ctx;
        pending_reload.client_ctx = client_ctx;
        pending_reload.metadata = new_metadata;
        pthread_mutex_unlock(&pending_reload_mutex);

        serverLog(LL_DEBUG, "Background TLS reload parsed TLS materials successfully");
    } else {
        if (tlsCreateContexts(ctx_config, &ctx, &client_ctx) == C_ERR) {
            return C_ERR;
        }

        SSL_CTX_free(valkey_tls_ctx);
        SSL_CTX_free(valkey_tls_client_ctx);
        valkey_tls_ctx = ctx;
        valkey_tls_client_ctx = client_ctx;
        captureMetadata(ctx_config, &active_metadata);
        tlsRefreshAllCertInfo();
    }

    atomic_store_explicit(&lastTlsConfigureTime, server.ustime, memory_order_relaxed);
    return C_OK;
}

/* Synchronous TLS configuration - blocks until complete.
 * Called from CONFIG SET commands and server initialization. */
static int tlsConfigureSync(void *priv, int reconfigure) {
    return tlsConfigure(priv, reconfigure, false);
}

/* Asynchronous TLS configuration - runs in background thread.
 * Does CPU-intensive certificate loading without blocking main thread.
 * The main thread will later call tlsApplyPendingReload() to swap in the new contexts. */
void tlsConfigureAsync(void) {
    tlsConfigure(&server.tls_ctx_config, 1, true);
}

/* This function runs in the main thread and applies the TLS contexts
 * that were prepared by the background thread atomically. This is a quick operation
 * that just swaps pointers, updates metadata, and frees old contexts. */
void tlsApplyPendingReload(void) {
    tlsPendingReload local_pending;
    pthread_mutex_lock(&pending_reload_mutex);
    if (!pending_reload.ctx) {
        pthread_mutex_unlock(&pending_reload_mutex);
        return;
    }

    if (!metadataChanged(&active_metadata, &pending_reload.metadata)) {
        SSL_CTX_free(pending_reload.ctx);
        SSL_CTX_free(pending_reload.client_ctx);
        memset(&pending_reload, 0, sizeof(pending_reload));
        pthread_mutex_unlock(&pending_reload_mutex);
        serverLog(LL_DEBUG, "Discarding pending TLS reload with unchanged materials");
        return;
    }

    local_pending = pending_reload;
    memset(&pending_reload, 0, sizeof(pending_reload));
    pthread_mutex_unlock(&pending_reload_mutex);

    SSL_CTX *old_ctx = valkey_tls_ctx;
    SSL_CTX *old_client_ctx = valkey_tls_client_ctx;

    valkey_tls_ctx = local_pending.ctx;
    valkey_tls_client_ctx = local_pending.client_ctx;

    active_metadata = local_pending.metadata;

    SSL_CTX_free(old_ctx);
    SSL_CTX_free(old_client_ctx);

    tlsRefreshAllCertInfo();

    serverLog(LL_NOTICE, "TLS materials reloaded successfully");
}

/* Check if it's time to trigger a background TLS reload check. */
void tlsReconfigureIfNeeded(void) {
    long long lastConfigureTime = atomic_load_explicit(&lastTlsConfigureTime, memory_order_relaxed);
    const long long configAgeMicros = server.ustime - lastConfigureTime;
    const long long configAgeSeconds = (configAgeMicros / 1000) / 1000;
    if (server.tls_ctx_config.auto_reload_interval == 0 ||
        configAgeSeconds < server.tls_ctx_config.auto_reload_interval) {
        return;
    }
    bioCreateTlsReloadJob();
}

static ConnectionType CT_TLS;

/* Normal socket connections have a simple events/handler correlation.
 *
 * With TLS connections we need to handle cases where during a logical read
 * or write operation, the SSL library asks to block for the opposite
 * socket operation.
 *
 * When this happens, we need to do two things:
 * 1. Make sure we register for the event.
 * 2. Make sure we know which handler needs to execute when the
 *    event fires.  That is, if we notify the caller of a write operation
 *    that it blocks, and SSL asks for a read, we need to trigger the
 *    write handler again on the next read event.
 *
 */

#define TLS_CONN_FLAG_READ_WANT_WRITE (1 << 0)
#define TLS_CONN_FLAG_WRITE_WANT_READ (1 << 1)
#define TLS_CONN_FLAG_FD_SET (1 << 2)
#define TLS_CONN_FLAG_POSTPONE_UPDATE_STATE (1 << 3)
#define TLS_CONN_FLAG_HAS_PENDING (1 << 4)
#define TLS_CONN_FLAG_ACCEPT_ERROR (1 << 5)
#define TLS_CONN_FLAG_ACCEPT_SUCCESS (1 << 6)

typedef struct tls_connection {
    connection c;
    int flags;
    SSL *ssl;
    char *ssl_error;
    listNode *pending_list_node;
    /* Per https://docs.openssl.org/master/man3/SSL_write, after a write call with partially written data,
     * we must make subsequent write calls with the same length. We use this field to keep track of
     * the previous write length. */
    size_t last_failed_write_data_len;
} tls_connection;

/* Fetch the latest OpenSSL error and store it in the connection */
static void updateTLSError(tls_connection *conn) {
    conn->c.last_errno = 0;
    if (conn->ssl_error) zfree(conn->ssl_error);
    conn->ssl_error = zmalloc(512);
    ERR_error_string_n(ERR_get_error(), conn->ssl_error, 512);
}

static connection *createTLSConnection(int client_side) {
    SSL_CTX *ctx = valkey_tls_ctx;
    if (client_side && valkey_tls_client_ctx) ctx = valkey_tls_client_ctx;
    tls_connection *conn = zcalloc(sizeof(tls_connection));
    conn->c.type = &CT_TLS;
    conn->c.fd = -1;
    conn->c.iovcnt = IOV_MAX;
    conn->ssl = SSL_new(ctx);
    if (!conn->ssl) {
        updateTLSError(conn);
        conn->c.state = CONN_STATE_ERROR;
    }
    return (connection *)conn;
}

static connection *connCreateTLS(void) {
    return createTLSConnection(1);
}

/* Create a new TLS connection that is already associated with
 * an accepted underlying file descriptor.
 *
 * The socket is not ready for I/O until connAccept() was called and
 * invoked the connection-level accept handler.
 *
 * Callers should use connGetState() and verify the created connection
 * is not in an error state.
 */
static connection *connCreateAcceptedTLS(int fd, void *priv) {
    int require_auth = *(int *)priv;
    tls_connection *conn = (tls_connection *)createTLSConnection(0);
    conn->c.fd = fd;
    if (conn->c.state == CONN_STATE_ERROR) return (connection *)conn;
    conn->c.state = CONN_STATE_ACCEPTING;

    switch (require_auth) {
    case TLS_CLIENT_AUTH_NO: SSL_set_verify(conn->ssl, SSL_VERIFY_NONE, NULL); break;
    case TLS_CLIENT_AUTH_OPTIONAL: SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, NULL); break;
    default: /* TLS_CLIENT_AUTH_YES, also fall-secure */
        SSL_set_verify(conn->ssl, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        break;
    }

    SSL_set_fd(conn->ssl, conn->c.fd);
    SSL_set_accept_state(conn->ssl);

    return (connection *)conn;
}

static int connTLSAccept(connection *_conn, ConnectionCallbackFunc accept_handler);
static void tlsEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask);
static void updateSSLEvent(tls_connection *conn);

static void clearTLSWantFlags(tls_connection *conn) {
    conn->flags &= ~(TLS_CONN_FLAG_WRITE_WANT_READ | TLS_CONN_FLAG_READ_WANT_WRITE);
}

/* Process the return code received from OpenSSL>
 * Update the conn flags with the WANT_READ/WANT_WRITE flags.
 * Update the connection's error state if a real error has occurred.
 * Returns an SSL error code, or 0 if no further handling is required.
 */
static int handleSSLReturnCode(tls_connection *conn, int ret_value) {
    clearTLSWantFlags(conn);
    if (ret_value <= 0) {
        int ssl_err = SSL_get_error(conn->ssl, ret_value);
        switch (ssl_err) {
        case SSL_ERROR_WANT_WRITE: conn->flags |= TLS_CONN_FLAG_READ_WANT_WRITE; return 0;
        case SSL_ERROR_WANT_READ: conn->flags |= TLS_CONN_FLAG_WRITE_WANT_READ; return 0;
        case SSL_ERROR_SYSCALL:
            conn->c.last_errno = errno;
            if (conn->ssl_error) zfree(conn->ssl_error);
            conn->ssl_error = errno ? zstrdup(strerror(errno)) : NULL;
            break;
        default:
            /* Error! */
            updateTLSError(conn);
            break;
        }

        return ssl_err;
    }

    return 0;
}

/* Handle OpenSSL return code following SSL_write() or SSL_read():
 *
 * - Updates conn state and last_errno.
 * - If update_event is nonzero, calls updateSSLEvent() when necessary.
 *
 * Returns ret_value, or -1 on error or dropped connection.
 */
static int updateStateAfterSSLIO(tls_connection *conn, int ret_value, int update_event) {
    /* If system call was interrupted, there's no need to go through the full
     * OpenSSL error handling and just report this for the caller to retry the
     * operation.
     */
    if (errno == EINTR) {
        conn->c.last_errno = EINTR;
        return -1;
    }

    if (ret_value <= 0) {
        int ssl_err;
        if (!(ssl_err = handleSSLReturnCode(conn, ret_value))) {
            if (update_event) updateSSLEvent(conn);
            errno = EAGAIN;
            return -1;
        } else {
            if (ssl_err == SSL_ERROR_ZERO_RETURN || ((ssl_err == SSL_ERROR_SYSCALL && !errno))) {
                conn->c.state = CONN_STATE_CLOSED;
                return 0;
            } else {
                conn->c.state = CONN_STATE_ERROR;
                return -1;
            }
        }
    }

    return ret_value;
}

static void registerSSLEvent(tls_connection *conn) {
    int mask = aeGetFileEvents(server.el, conn->c.fd);

    if (conn->flags & TLS_CONN_FLAG_WRITE_WANT_READ) {
        if (mask & AE_WRITABLE) aeDeleteFileEvent(server.el, conn->c.fd, AE_WRITABLE);
        if (!(mask & AE_READABLE)) aeCreateFileEvent(server.el, conn->c.fd, AE_READABLE, tlsEventHandler, conn);
    } else if (conn->flags & TLS_CONN_FLAG_READ_WANT_WRITE) {
        if (mask & AE_READABLE) aeDeleteFileEvent(server.el, conn->c.fd, AE_READABLE);
        if (!(mask & AE_WRITABLE)) aeCreateFileEvent(server.el, conn->c.fd, AE_WRITABLE, tlsEventHandler, conn);
    } else {
        serverAssert(0);
    }
}

static void postPoneUpdateSSLState(connection *conn_, int postpone) {
    tls_connection *conn = (tls_connection *)conn_;
    if (postpone) {
        conn->flags |= TLS_CONN_FLAG_POSTPONE_UPDATE_STATE;
    } else {
        conn->flags &= ~TLS_CONN_FLAG_POSTPONE_UPDATE_STATE;
    }
}

static void updatePendingData(tls_connection *conn) {
    if (conn->flags & TLS_CONN_FLAG_POSTPONE_UPDATE_STATE) return;

    /* If SSL has pending data, already read from the socket, we're at risk of not calling the read handler again, make
     * sure to add it to a list of pending connection that should be handled anyway. */
    if (conn->flags & TLS_CONN_FLAG_HAS_PENDING) {
        if (!conn->pending_list_node) {
            listAddNodeTail(pending_list, conn);
            conn->pending_list_node = listLast(pending_list);
        }
    } else if (conn->pending_list_node) {
        listDelNode(pending_list, conn->pending_list_node);
        conn->pending_list_node = NULL;
    }
}

void updateSSLPendingFlag(tls_connection *conn) {
    if (SSL_pending(conn->ssl) > 0) {
        conn->flags |= TLS_CONN_FLAG_HAS_PENDING;
    } else {
        conn->flags &= ~TLS_CONN_FLAG_HAS_PENDING;
    }
}

static void updateSSLEvent(tls_connection *conn) {
    if (conn->flags & TLS_CONN_FLAG_POSTPONE_UPDATE_STATE) return;

    int mask = aeGetFileEvents(server.el, conn->c.fd);
    int need_read = conn->c.read_handler || (conn->flags & TLS_CONN_FLAG_WRITE_WANT_READ);
    int need_write = conn->c.write_handler || (conn->flags & TLS_CONN_FLAG_READ_WANT_WRITE);

    if (need_read && !(mask & AE_READABLE))
        aeCreateFileEvent(server.el, conn->c.fd, AE_READABLE, tlsEventHandler, conn);
    if (!need_read && (mask & AE_READABLE)) aeDeleteFileEvent(server.el, conn->c.fd, AE_READABLE);

    if (need_write && !(mask & AE_WRITABLE))
        aeCreateFileEvent(server.el, conn->c.fd, AE_WRITABLE, tlsEventHandler, conn);
    if (!need_write && (mask & AE_WRITABLE)) aeDeleteFileEvent(server.el, conn->c.fd, AE_WRITABLE);
}

static int TLSHandleAcceptResult(tls_connection *conn, int call_handler_on_error) {
    serverAssert(conn->c.state == CONN_STATE_ACCEPTING);
    if (conn->flags & TLS_CONN_FLAG_ACCEPT_SUCCESS) {
        conn->c.state = CONN_STATE_CONNECTED;
    } else if (conn->flags & TLS_CONN_FLAG_ACCEPT_ERROR) {
        conn->c.state = CONN_STATE_ERROR;
        if (!call_handler_on_error) return C_ERR;
    } else {
        /* Still pending accept */
        registerSSLEvent(conn);
        return C_OK;
    }

    /* call accept handler */
    if (!callHandler((connection *)conn, conn->c.conn_handler)) return C_ERR;
    conn->c.conn_handler = NULL;
    return C_OK;
}

static void updateSSLState(connection *conn_) {
    tls_connection *conn = (tls_connection *)conn_;

    if (conn->c.state == CONN_STATE_ACCEPTING) {
        if (TLSHandleAcceptResult(conn, 1) == C_ERR || conn->c.state != CONN_STATE_CONNECTED) return;
    }

    updateSSLEvent(conn);
    updatePendingData(conn);
}

static int getCertSubjectFieldByName(X509 *cert, const char *field, char *out, size_t outlen) {
    if (!cert || !field || !out) return 0;

    int nid = -1;

    if (!strcasecmp(field, "CN"))
        nid = NID_commonName;
    else if (!strcasecmp(field, "O"))
        nid = NID_organizationName;
    /* Add more mappings here as needed */

    if (nid == -1) return 0;

    X509_NAME *subject = X509_get_subject_name(cert);
    if (!subject) return 0;

    return X509_NAME_get_text_by_NID(subject, nid, out, outlen) > 0;
}

/* Extract URI from Subject Alternative Name extension and return the first
 * enabled Valkey user that matches a URI. Returns NULL if no match found.
 * If cert_username is non-NULL, it is set to the last URI checked. */
static user *getValidUserFromCertSanUri(X509 *cert, sds *cert_username) {
    if (!cert) return NULL;

    GENERAL_NAMES *san_names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    if (!san_names) return NULL;

    user *result = NULL;
    int num_names = sk_GENERAL_NAME_num(san_names);

    for (int i = 0; i < num_names; i++) {
        GENERAL_NAME *name = sk_GENERAL_NAME_value(san_names, i);

        if (name->type == GEN_URI) {
            ASN1_STRING *uri_asn1 = name->d.uniformResourceIdentifier;
            const unsigned char *uri_data = ASN1_STRING_get0_data(uri_asn1);
            int uri_len = ASN1_STRING_length(uri_asn1);

            if (!uri_data || uri_len <= 0 || memchr(uri_data, '\0', uri_len)) {
                serverLog(LL_DEBUG, "TLS: Invalid or malformed SAN URI in certificate");
                continue;
            }

            if (cert_username) {
                sdsfree(*cert_username);
                *cert_username = sdsnewlen(uri_data, uri_len);
            }

            user *u = ACLGetUserByName((const char *)uri_data, uri_len);
            if (u && (u->flags & USER_FLAG_ENABLED)) {
                result = u;
                break;
            }
        }
    }

    GENERAL_NAMES_free(san_names);
    return result;
}

user *tlsGetPeerUser(connection *conn_, sds *cert_username) {
    tls_connection *conn = (tls_connection *)conn_;
    if (!conn || !SSL_is_init_finished(conn->ssl)) return NULL;

    long verify_result = SSL_get_verify_result(conn->ssl);
    if (verify_result != X509_V_OK) {
        serverLog(LL_DEBUG, "TLS: Client certificate verification failed: %s",
                  X509_verify_cert_error_string(verify_result));
        return NULL;
    }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    X509 *cert = SSL_get0_peer_certificate(conn->ssl);
#else
    X509 *cert = SSL_get_peer_certificate(conn->ssl);
#endif
    if (!cert) return NULL;

    user *result = NULL;

    switch (server.tls_ctx_config.client_auth_user) {
    case TLS_CLIENT_FIELD_URI:
        result = getValidUserFromCertSanUri(cert, cert_username);
        if (!result) {
            serverLog(LL_VERBOSE, "TLS: No matching user found in certificate SAN URI fields");
        }
        break;

    case TLS_CLIENT_FIELD_CN: {
        char field_value[256];
        if (getCertSubjectFieldByName(cert, "CN", field_value, sizeof(field_value))) {
            if (cert_username) *cert_username = sdsnew(field_value);
            result = ACLGetUserByName(field_value, strlen(field_value));
            if (!result || !(result->flags & USER_FLAG_ENABLED)) {
                serverLog(LL_VERBOSE, "TLS: No matching user found for certificate CN '%s'", field_value);
                result = NULL;
            }
        } else {
            serverLog(LL_DEBUG, "TLS: Failed to extract CN in certificate subject");
        }
        break;
    }

    default:
        break;
    }

#if OPENSSL_VERSION_NUMBER < 0x30000000L
    X509_free(cert);
#endif

    return result;
}

static void TLSAccept(void *_conn) {
    tls_connection *conn = (tls_connection *)_conn;
    ERR_clear_error();
    int ret = SSL_accept(conn->ssl);
    if (ret > 0) {
        conn->flags |= TLS_CONN_FLAG_ACCEPT_SUCCESS;
    } else if (handleSSLReturnCode(conn, ret)) {
        conn->flags |= TLS_CONN_FLAG_ACCEPT_ERROR;
    }
}

static void tlsHandleEvent(tls_connection *conn, int mask) {
    int ret, conn_error;

    switch (conn->c.state) {
    case CONN_STATE_CONNECTING:
        conn_error = anetGetError(conn->c.fd);
        if (conn_error) {
            conn->c.last_errno = conn_error;
            conn->c.state = CONN_STATE_ERROR;
        } else {
            ERR_clear_error();
            if (!(conn->flags & TLS_CONN_FLAG_FD_SET)) {
                SSL_set_fd(conn->ssl, conn->c.fd);
                conn->flags |= TLS_CONN_FLAG_FD_SET;
            }
            ret = SSL_connect(conn->ssl);
            if (ret <= 0) {
                if (!handleSSLReturnCode(conn, ret)) {
                    registerSSLEvent(conn);
                    /* Avoid hitting UpdateSSLEvent, which knows nothing
                     * of what SSL_connect() wants and instead looks at our
                     * R/W handlers.
                     */
                    return;
                }

                /* If not handled, it's an error */
                conn->c.state = CONN_STATE_ERROR;
            } else {
                conn->c.state = CONN_STATE_CONNECTED;
            }
        }

        if (!callHandler((connection *)conn, conn->c.conn_handler)) return;
        conn->c.conn_handler = NULL;
        break;
    case CONN_STATE_ACCEPTING:
        if (connTLSAccept((connection *)conn, NULL) == C_ERR || conn->c.state != CONN_STATE_CONNECTED) return;
        break;
    case CONN_STATE_CONNECTED: {
        int call_read = ((mask & AE_READABLE) && conn->c.read_handler) ||
                        ((mask & AE_WRITABLE) && (conn->flags & TLS_CONN_FLAG_READ_WANT_WRITE));
        int call_write = ((mask & AE_WRITABLE) && conn->c.write_handler) ||
                         ((mask & AE_READABLE) && (conn->flags & TLS_CONN_FLAG_WRITE_WANT_READ));

        /* Normally we execute the readable event first, and the writable
         * event laster. This is useful as sometimes we may be able
         * to serve the reply of a query immediately after processing the
         * query.
         *
         * However if WRITE_BARRIER is set in the mask, our application is
         * asking us to do the reverse: never fire the writable event
         * after the readable. In such a case, we invert the calls.
         * This is useful when, for instance, we want to do things
         * in the beforeSleep() hook, like fsynching a file to disk,
         * before replying to a client. */
        int invert = conn->c.flags & CONN_FLAG_WRITE_BARRIER;

        if (!invert && call_read) {
            if (!callHandler((connection *)conn, conn->c.read_handler)) return;
        }

        /* Fire the writable event. */
        if (call_write) {
            if (!callHandler((connection *)conn, conn->c.write_handler)) return;
        }

        /* If we have to invert the call, fire the readable event now
         * after the writable one. */
        if (invert && call_read) {
            if (!callHandler((connection *)conn, conn->c.read_handler)) return;
        }
        updatePendingData(conn);

        break;
    }
    default: break;
    }

    updateSSLEvent(conn);
}

static void tlsEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    UNUSED(el);
    UNUSED(fd);
    tls_connection *conn = clientData;
    tlsHandleEvent(conn, mask);
}

static void tlsAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = server.max_new_tls_conns_per_cycle;
    char cip[NET_IP_STR_LEN];
    struct ClientFlags flags = {0};
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while (max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (anetRetryAcceptOnError(errno)) continue;
            if (errno != EWOULDBLOCK) serverLog(LL_WARNING, "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE, "Accepted %s:%d", cip, cport);

        if (server.tcpkeepalive) anetKeepAlive(NULL, cfd, server.tcpkeepalive);
        acceptCommonHandler(connCreateAcceptedTLS(cfd, &server.tls_auth_clients), flags, cip);
    }
}

static int connTLSAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    return anetFdToString(conn->fd, ip, ip_len, port, remote);
}

static int connTLSIsLocal(connection *conn) {
    return connectionTypeTcp()->is_local(conn);
}

static int connTLSListen(connListener *listener) {
    return listenToPort(listener);
}

static int connTLSIsIntegrityChecked(void) {
    return 1;
}

static void connTLSCloseListener(connListener *listener) {
    connectionTypeTcp()->closeListener(listener);
}

static void connTLSShutdown(connection *conn_) {
    tls_connection *conn = (tls_connection *)conn_;

    if (conn->ssl) {
        if (conn->c.state == CONN_STATE_CONNECTED) SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }

    connectionTypeTcp()->shutdown(conn_);
}

static void connTLSClose(connection *conn_) {
    tls_connection *conn = (tls_connection *)conn_;

    if (conn->ssl) {
        if (conn->c.state == CONN_STATE_CONNECTED) SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }

    if (conn->ssl_error) {
        zfree(conn->ssl_error);
        conn->ssl_error = NULL;
    }

    if (conn->pending_list_node) {
        listDelNode(pending_list, conn->pending_list_node);
        conn->pending_list_node = NULL;
    }

    connectionTypeTcp()->close(conn_);
}

static int connTLSAccept(connection *_conn, ConnectionCallbackFunc accept_handler) {
    tls_connection *conn = (tls_connection *)_conn;
    if (conn->c.state != CONN_STATE_ACCEPTING) return C_ERR;
    int call_handler_on_error = 1;
    /* Try to accept */
    if (accept_handler) {
        conn->c.conn_handler = accept_handler;
        call_handler_on_error = 0;
    }

    /* We're in IO thread - just call accept and return, the main thread will handle the rest */
    if (!inMainThread()) {
        TLSAccept(conn);
        return C_OK;
    }

    /* Try to offload accept to IO threads */
    if (trySendAcceptToIOThreads(_conn) == C_OK) return C_OK;

    TLSAccept(conn);
    return TLSHandleAcceptResult(conn, call_handler_on_error);
}

static int connTLSConnect(connection *conn_,
                          const char *addr,
                          int port,
                          const char *src_addr,
                          int multipath,
                          ConnectionCallbackFunc connect_handler) {
    tls_connection *conn = (tls_connection *)conn_;
    unsigned char addr_buf[sizeof(struct in6_addr)];

    if (conn->c.state != CONN_STATE_NONE) return C_ERR;
    ERR_clear_error();

    /* Check whether addr is an IP address, if not, use the value for Server Name Indication */
    if (inet_pton(AF_INET, addr, addr_buf) != 1 && inet_pton(AF_INET6, addr, addr_buf) != 1) {
        SSL_set_tlsext_host_name(conn->ssl, addr);
    }

    /* Initiate Socket connection first */
    if (connectionTypeTcp()->connect(conn_, addr, port, src_addr, multipath, connect_handler) == C_ERR) return C_ERR;

    /* Return now, once the socket is connected we'll initiate
     * TLS connection from the event handler.
     */
    return C_OK;
}

static int connTLSWrite(connection *conn_, const void *data, size_t data_len) {
    tls_connection *conn = (tls_connection *)conn_;
    int ret;

    if (conn->c.state != CONN_STATE_CONNECTED) return -1;
    ERR_clear_error();
    /* In case when last write failed due to some internal reason, retry has to provide
     * at least the same amount of bytes (https://docs.openssl.org/master/man3/SSL_write).
     * If that condition is not met, OpenSSL will return "SSL routines::bad length".
     * Currently we only suspect this can happen during primary cron sending '\n'
     * indication to the replica, so we silently return from this function without
     * impacting the connection state. */
    if (data_len < conn->last_failed_write_data_len) {
        // TODO: place debugAssert for this case once the known issue described is resolved
        return -1;
    }
    ret = SSL_write(conn->ssl, data, data_len);
    conn->last_failed_write_data_len = ret <= 0 ? data_len : 0;
    return updateStateAfterSSLIO(conn, ret, 1);
}

static int connTLSWritev(connection *conn_, const struct iovec *iov, int iovcnt) {
    tls_connection *conn = (tls_connection *)conn_;
    if (iovcnt == 1) return connTLSWrite(conn_, iov[0].iov_base, iov[0].iov_len);

    /* Accumulate the amount of bytes of each buffer and check if it exceeds NET_MAX_WRITES_PER_EVENT. */
    size_t iov_bytes_len = 0;
    for (int i = 0; i < iovcnt; i++) {
        iov_bytes_len += iov[i].iov_len;
        if (iov_bytes_len > NET_MAX_WRITES_PER_EVENT) break;
    }

    /* In case the amount of all buffers is greater than NET_MAX_WRITES_PER_EVENT,
     * it might not worth doing so much memory copying to reduce system calls,
     * therefore, invoke connTLSWrite() multiple times to avoid memory copies.
     * However, in case when last write failed we still have to repeat sending last_failed_write_data_len
     * bytes. Because of openssl implementation we cannot repeat sending writes with length smaller than
     * the last failed write (https://docs.openssl.org/master/man3/SSL_write) so in case the first io buffer
     * does not provide at least the same amount of bytes as previous failed write, we will have to fallback to
     * memory copy to a static buffer before calling SSL_write. */
    if (iov_bytes_len > NET_MAX_WRITES_PER_EVENT && iovcnt > 0 && iov[0].iov_len >= conn->last_failed_write_data_len) {
        ssize_t tot_sent = 0;
        for (int i = 0; i < iovcnt; i++) {
            ssize_t sent = connTLSWrite(conn_, iov[i].iov_base, iov[i].iov_len);
            if (sent <= 0) return tot_sent > 0 ? tot_sent : sent;
            tot_sent += sent;
            if ((size_t)sent != iov[i].iov_len) break;
        }
        return tot_sent;
    }

    /* The amount of all buffers is less than NET_MAX_WRITES_PER_EVENT,
     * which is worth doing more memory copies in exchange for fewer system calls,
     * so concatenate these scattered buffers into a contiguous piece of memory
     * and send it away by one call to connTLSWrite().
     * However, code can fallback here in case when last write failed and first
     * element of io is buffer not big enough to provide required amount of bytes
     * to retry, so iov_bytes_len may exceed NET_MAX_WRITES_PER_EVENT by the amount
     * of remaining bytes from last taken io. */
    char buf[iov_bytes_len];
    size_t offset = 0;
    for (int i = 0; i < iovcnt && offset < iov_bytes_len; i++) {
        memcpy(buf + offset, iov[i].iov_base, iov[i].iov_len);
        offset += iov[i].iov_len;
    }
    return connTLSWrite(conn_, buf, iov_bytes_len);
}

static int connTLSRead(connection *conn_, void *buf, size_t buf_len) {
    tls_connection *conn = (tls_connection *)conn_;
    int ret;

    if (conn->c.state != CONN_STATE_CONNECTED) return -1;
    ERR_clear_error();
    ret = SSL_read(conn->ssl, buf, buf_len);
    updateSSLPendingFlag(conn);
    return updateStateAfterSSLIO(conn, ret, 1);
}

static const char *connTLSGetLastError(connection *conn_) {
    tls_connection *conn = (tls_connection *)conn_;

    if (conn->ssl_error) return conn->ssl_error;
    /* If no SSL error is set, return the last errno string. */
    return strerror(conn_->last_errno);
}

static int connTLSSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    conn->write_handler = func;
    if (barrier)
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    else
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;
    updateSSLEvent((tls_connection *)conn);
    return C_OK;
}

static int connTLSSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    conn->read_handler = func;
    updateSSLEvent((tls_connection *)conn);
    return C_OK;
}

static int isBlocking(tls_connection *conn) {
    return anetIsBlock(NULL, conn->c.fd);
}

static void setBlockingTimeout(tls_connection *conn, long long timeout) {
    anetBlock(NULL, conn->c.fd);
    anetSendTimeout(NULL, conn->c.fd, timeout);
    anetRecvTimeout(NULL, conn->c.fd, timeout);
}

static void unsetBlockingTimeout(tls_connection *conn) {
    anetNonBlock(NULL, conn->c.fd);
    anetSendTimeout(NULL, conn->c.fd, 0);
    anetRecvTimeout(NULL, conn->c.fd, 0);
}

static int connTLSBlockingConnect(connection *conn_, const char *addr, int port, long long timeout) {
    tls_connection *conn = (tls_connection *)conn_;
    int ret;

    if (conn->c.state != CONN_STATE_NONE) return C_ERR;

    /* Initiate socket blocking connect first */
    if (connectionTypeTcp()->blocking_connect(conn_, addr, port, timeout) == C_ERR) return C_ERR;

    /* Initiate TLS connection now.  We set up a send/recv timeout on the socket,
     * which means the specified timeout will not be enforced accurately. */
    SSL_set_fd(conn->ssl, conn->c.fd);
    setBlockingTimeout(conn, timeout);
    ERR_clear_error();
    if ((ret = SSL_connect(conn->ssl)) <= 0) {
        conn->c.state = CONN_STATE_ERROR;
        return C_ERR;
    }
    unsetBlockingTimeout(conn);

    conn->c.state = CONN_STATE_CONNECTED;
    return C_OK;
}

static ssize_t connTLSSyncWrite(connection *conn_, char *ptr, ssize_t size, long long timeout) {
    tls_connection *conn = (tls_connection *)conn_;
    int blocking = isBlocking(conn);
    setBlockingTimeout(conn, timeout);
    SSL_clear_mode(conn->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
    ERR_clear_error();
    int ret = SSL_write(conn->ssl, ptr, size);
    ret = updateStateAfterSSLIO(conn, ret, 0);
    SSL_set_mode(conn->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
    if (!blocking) {
        unsetBlockingTimeout(conn);
    }

    return ret;
}

static ssize_t connTLSSyncRead(connection *conn_, char *ptr, ssize_t size, long long timeout) {
    tls_connection *conn = (tls_connection *)conn_;
    int blocking = isBlocking(conn);
    setBlockingTimeout(conn, timeout);
    ERR_clear_error();
    int ret = SSL_read(conn->ssl, ptr, size);
    updateSSLPendingFlag(conn);
    ret = updateStateAfterSSLIO(conn, ret, 0);
    if (!blocking) {
        unsetBlockingTimeout(conn);
    }

    return ret;
}

static ssize_t connTLSSyncReadLine(connection *conn_, char *ptr, ssize_t size, long long timeout) {
    tls_connection *conn = (tls_connection *)conn_;
    ssize_t nread = 0;

    int blocking = isBlocking(conn);
    setBlockingTimeout(conn, timeout);

    size--;
    while (size) {
        char c;

        ERR_clear_error();
        int ret = SSL_read(conn->ssl, &c, 1);
        updateSSLPendingFlag(conn);
        ret = updateStateAfterSSLIO(conn, ret, 0);
        if (ret <= 0) {
            nread = -1;
            goto exit;
        }
        if (c == '\n') {
            *ptr = '\0';
            if (nread && *(ptr - 1) == '\r') *(ptr - 1) = '\0';
            goto exit;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }
        size--;
    }
exit:
    if (!blocking) {
        unsetBlockingTimeout(conn);
    }
    return nread;
}

static int connTLSGetType(void) {
    return CONN_TYPE_TLS;
}

static int tlsHasPendingData(void) {
    if (!pending_list) return 0;
    return listLength(pending_list) > 0;
}

static int tlsProcessPendingData(void) {
    listIter li;
    listNode *ln;

    int processed = 0;
    listRewind(pending_list, &li);
    while ((ln = listNext(&li))) {
        tls_connection *conn = listNodeValue(ln);
        if (conn->flags & TLS_CONN_FLAG_POSTPONE_UPDATE_STATE) continue;
        tlsHandleEvent(conn, AE_READABLE);
        processed++;
    }
    return processed;
}

/* Fetch the peer certificate used for authentication on the specified
 * connection and return it as a PEM-encoded sds.
 */
static sds connTLSGetPeerCert(connection *conn_) {
    tls_connection *conn = (tls_connection *)conn_;
    if ((conn_->type != connectionTypeTls()) || !conn->ssl) return NULL;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    X509 *cert = SSL_get0_peer_certificate(conn->ssl);
#else
    X509 *cert = SSL_get_peer_certificate(conn->ssl);
#endif
    if (!cert) return NULL;

    BIO *bio = BIO_new(BIO_s_mem());
    if (bio == NULL || !PEM_write_bio_X509(bio, cert)) {
        if (bio != NULL) BIO_free(bio);
#if OPENSSL_VERSION_NUMBER < 0x30000000L
        X509_free(cert);
#endif
        return NULL;
    }

    const char *bio_ptr;
    long long bio_len = BIO_get_mem_data(bio, &bio_ptr);
    sds cert_pem = sdsnewlen(bio_ptr, bio_len);
    BIO_free(bio);

#if OPENSSL_VERSION_NUMBER < 0x30000000L
    X509_free(cert);
#endif

    return cert_pem;
}

static ConnectionType CT_TLS = {
    /* connection type */
    .get_type = connTLSGetType,

    /* connection type initialize & finalize & configure */
    .init = tlsInit,
    .cleanup = tlsCleanup,
    .configure = tlsConfigureSync,

    /* ae & accept & listen & error & address handler */
    .ae_handler = tlsEventHandler,
    .accept_handler = tlsAcceptHandler,
    .addr = connTLSAddr,
    .is_local = connTLSIsLocal,
    .listen = connTLSListen,
    .closeListener = connTLSCloseListener,

    /* create/shutdown/close connection */
    .conn_create = connCreateTLS,
    .conn_create_accepted = connCreateAcceptedTLS,
    .shutdown = connTLSShutdown,
    .close = connTLSClose,

    /* connect & accept */
    .connect = connTLSConnect,
    .blocking_connect = connTLSBlockingConnect,
    .accept = connTLSAccept,

    /* IO */
    .read = connTLSRead,
    .write = connTLSWrite,
    .writev = connTLSWritev,
    .set_write_handler = connTLSSetWriteHandler,
    .set_read_handler = connTLSSetReadHandler,
    .get_last_error = connTLSGetLastError,
    .sync_write = connTLSSyncWrite,
    .sync_read = connTLSSyncRead,
    .sync_readline = connTLSSyncReadLine,

    /* pending data */
    .has_pending_data = tlsHasPendingData,
    .process_pending_data = tlsProcessPendingData,
    .postpone_update_state = postPoneUpdateSSLState,
    .update_state = updateSSLState,

    /* TLS specified methods */
    .get_peer_cert = connTLSGetPeerCert,
    .get_peer_user = tlsGetPeerUser,

    /* Miscellaneous */
    .connIntegrityChecked = connTLSIsIntegrityChecked,

};

int RedisRegisterConnectionTypeTLS(void) {
    return connTypeRegister(&CT_TLS);
}

#else /* USE_OPENSSL */

static void tlsClearAllCertInfo(void);

void tlsResetCertInfo(void) {
    if (server.tls_port || server.tls_replication || server.tls_cluster) return;
    tlsClearAllCertInfo();
}

int RedisRegisterConnectionTypeTLS(void) {
    serverLog(LL_VERBOSE, "Connection type %s not builtin", getConnectionTypeName(CONN_TYPE_TLS));
    return C_ERR;
}

#endif

static void tlsClearCertInfo(long long *expiry, sds *serial) {
    if (expiry) *expiry = 0;
    if (serial && *serial) {
        sdsfree(*serial);
        *serial = NULL;
    }
}

static void tlsClearCACertInfo(void) {
    tlsClearCertInfo(&server.tls_ca_cert_expire_time, &server.tls_ca_cert_serial);
}

static void tlsClearAllCertInfo(void) {
    tlsClearCertInfo(&server.tls_server_cert_expire_time, &server.tls_server_cert_serial);
    tlsClearCertInfo(&server.tls_client_cert_expire_time, &server.tls_client_cert_serial);
    tlsClearCACertInfo();
}

#if defined(BUILD_TLS_MODULE) && BUILD_TLS_MODULE == 2 /* BUILD_MODULE */

#include "release.h"

int ValkeyModule_OnLoad(void *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    /* Connection modules must be part of the same build as the server. */
    if (strcmp(REDIS_BUILD_ID_RAW, serverBuildIdRaw())) {
        serverLog(LL_NOTICE, "Connection type %s was not built together with the valkey-server used.", getConnectionTypeName(CONN_TYPE_TLS));
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_Init(ctx, "tls", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    /* Connection modules is available only bootup. */
    if ((ValkeyModule_GetContextFlags(ctx) & VALKEYMODULE_CTX_FLAGS_SERVER_STARTUP) == 0) {
        serverLog(LL_NOTICE, "Connection type %s can be loaded only during bootup", getConnectionTypeName(CONN_TYPE_TLS));
        return VALKEYMODULE_ERR;
    }

    ValkeyModule_SetModuleOptions(ctx, VALKEYMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD | VALKEYMODULE_OPTIONS_HANDLE_ATOMIC_SLOT_MIGRATION);

    if (connTypeRegister(&CT_TLS) != C_OK) return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(void *arg) {
    UNUSED(arg);
    serverLog(LL_NOTICE, "Connection type %s can not be unloaded", getConnectionTypeName(CONN_TYPE_TLS));
    return VALKEYMODULE_ERR;
}
#endif
