/* SPDX-License-Identifier: GPL-2.0 */
/* Independent OpenSSL client against the real Mbed TLS server engine. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ssl.h>
#include <openssl/ssl.h>

#include "../src/tls/tls_engine.h"
#include "gxfp/tls/tls_constants.h"

#define CHECK(x) do {							\
		if (!(x)) {						\
			fprintf(stderr, "line %d: %s\n", __LINE__, #x);	\
			exit(1);					\
		}							\
	} while (0)

struct test_key {
	unsigned char bytes[48];
	unsigned int len;
};

static unsigned int psk_cb(SSL *s, const char *hint, char *id, unsigned int id_cap,
			   unsigned char *psk, unsigned int psk_cap)
{
	struct test_key *k = SSL_get_app_data(s);

	(void)hint;
	if (id_cap < sizeof(GXFP5130_PSK_IDENTITY) || psk_cap < k->len)
		return 0;
	memcpy(id, GXFP5130_PSK_IDENTITY, sizeof(GXFP5130_PSK_IDENTITY));
	memcpy(psk, k->bytes, k->len);
	return k->len;
}

static int server_send(void *ctx, const uint8_t *b, size_t n)
{
	return BIO_write(ctx, b, n) == (int)n ? 0 : -EIO;
}

static void handshake(unsigned int len, int wrong_key)
{
	struct test_key k = { .len = len };
	struct gxfp_tls_engine *e = NULL;
	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
	BIO *in = BIO_new(BIO_s_mem()), *out = BIO_new(BIO_s_mem());
	unsigned char buf[16384];
	int server_done = 0, client_done = 0, failed = 0;
	SSL *s;
	int n;

	CHECK(ctx && in && out);
	CHECK(SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION));
	CHECK(SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION));
	CHECK(SSL_CTX_set_cipher_list(ctx, "PSK-AES128-CBC-SHA256"));
	/* The sensor uses neither extended master secret nor encrypt-then-MAC. */
	SSL_CTX_set_options(ctx, SSL_OP_NO_EXTENDED_MASTER_SECRET |
				 SSL_OP_NO_ENCRYPT_THEN_MAC | SSL_OP_NO_TICKET);
	SSL_CTX_set_psk_client_callback(ctx, psk_cb);
	s = SSL_new(ctx);
	CHECK(s);
	SSL_set_bio(s, in, out);
	SSL_set_connect_state(s);
	SSL_set_app_data(s, &k);
	BIO_set_mem_eof_return(in, -1);

	for (unsigned int i = 0; i < len; i++)
		k.bytes[i] = (unsigned char)(i * 7 + 3);
	CHECK(gxfp_tls_engine_create(&e, k.bytes, len, server_send, in) == 0);
	if (wrong_key)
		k.bytes[len - 1] ^= 0x80;

	for (int step = 0; step < 100 && !failed && !(server_done && client_done); step++) {
		int r = SSL_do_handshake(s);

		if (r == 1) {
			client_done = 1;
		} else {
			int err = SSL_get_error(s, r);

			if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
				failed = 1;
		}
		while (BIO_ctrl_pending(out)) {
			n = BIO_read(out, buf, sizeof(buf));
			CHECK(n > 0);
			CHECK(gxfp_tls_engine_feed_cipher_record(e, buf, n) == 0);
		}
		r = gxfp_tls_engine_handshake_step(e);
		if (r == 1)
			server_done = 1;
		else if (r != -EAGAIN)
			failed = 1;
	}

	if (wrong_key) {
		CHECK(failed && !server_done);
	} else {
		size_t plain_len = 0;

		CHECK(!failed && server_done && client_done);
		CHECK(SSL_write(s, "authenticated", 13) == 13);
		n = BIO_read(out, buf, sizeof(buf));
		CHECK(n > 0);
		CHECK(gxfp_tls_engine_feed_cipher_record(e, buf, n) == 0);
		CHECK(gxfp_tls_engine_pull_plain(e, buf, sizeof(buf), &plain_len) == 1);
		CHECK(plain_len == 13 && !memcmp(buf, "authenticated", 13));
	}
	gxfp_tls_engine_free(e);
	SSL_free(s);
	SSL_CTX_free(ctx);
}

int main(void)
{
	struct gxfp_tls_engine *e = NULL;
	uint8_t key[49] = { 0 };
	const size_t invalid[] = { 0, 1, 31, 33, 47, 49 };

	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		CHECK(gxfp_tls_engine_create(&e, key, invalid[i], server_send, NULL) == -EINVAL);

	handshake(32, 0);
#if MBEDTLS_PSK_MAX_LEN >= 48
	handshake(48, 0);
	handshake(48, 1);
	puts("PSK32/PSK48 authenticated data, wrong-key rejection, length validation: PASS");
#else
	CHECK(gxfp_tls_engine_create(&e, key, 48, server_send, NULL) == -EOPNOTSUPP);
	puts("PSK32 authenticated data, length validation: PASS; PSK48 refused: MBEDTLS_PSK_MAX_LEN < 48");
#endif
	return 0;
}
