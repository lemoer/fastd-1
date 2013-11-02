/*
  Copyright (c) 2012-2013, Matthias Schiffer <mschiffer@universe-factory.net>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "../../fastd.h"
#include "../common.h"

#include <crypto_secretbox_xsalsa20poly1305.h>


struct fastd_method_session_state {
	fastd_method_common_t common;

	uint8_t key[crypto_secretbox_xsalsa20poly1305_KEYBYTES];
};


static bool method_provides(const char *name) {
	return !strcmp(name, "xsalsa20-poly1305");
}

static size_t method_max_packet_size(fastd_context_t *ctx) {
	return (fastd_max_packet_size(ctx) + COMMON_NONCEBYTES + crypto_secretbox_xsalsa20poly1305_ZEROBYTES - crypto_secretbox_xsalsa20poly1305_BOXZEROBYTES);
}

static size_t method_min_encrypt_head_space(fastd_context_t *ctx UNUSED) {
	return crypto_secretbox_xsalsa20poly1305_ZEROBYTES;
}

static size_t method_min_decrypt_head_space(fastd_context_t *ctx UNUSED) {
	return (crypto_secretbox_xsalsa20poly1305_BOXZEROBYTES - COMMON_NONCEBYTES);
}

static size_t method_min_tail_space(fastd_context_t *ctx UNUSED) {
	return 0;
}


static size_t method_key_length(fastd_context_t *ctx UNUSED) {
	return crypto_secretbox_xsalsa20poly1305_KEYBYTES;
}

static fastd_method_session_state_t* method_session_init(fastd_context_t *ctx, const char *name UNUSED, const uint8_t *secret, bool initiator) {
	fastd_method_session_state_t *session = malloc(sizeof(fastd_method_session_state_t));

	fastd_method_common_init(ctx, &session->common, initiator);

	memcpy(session->key, secret, crypto_secretbox_xsalsa20poly1305_KEYBYTES);

	return session;
}

static fastd_method_session_state_t* method_session_init_compat(fastd_context_t *ctx, const char *name, const uint8_t *secret, size_t length, bool initiator) {
	if (length < crypto_secretbox_xsalsa20poly1305_KEYBYTES)
		exit_bug(ctx, "xsalsa20-poly1305: tried to init with short secret");

	return method_session_init(ctx, name, secret, initiator);
}

static bool method_session_is_valid(fastd_context_t *ctx, fastd_method_session_state_t *session) {
	return (session && fastd_method_session_common_is_valid(ctx, &session->common));
}

static bool method_session_is_initiator(fastd_context_t *ctx UNUSED, fastd_method_session_state_t *session) {
	return fastd_method_session_common_is_initiator(&session->common);
}

static bool method_session_want_refresh(fastd_context_t *ctx, fastd_method_session_state_t *session) {
	return fastd_method_session_common_want_refresh(ctx, &session->common);
}

static void method_session_superseded(fastd_context_t *ctx, fastd_method_session_state_t *session) {
	fastd_method_session_common_superseded(ctx, &session->common);
}

static void method_session_free(fastd_context_t *ctx UNUSED, fastd_method_session_state_t *session) {
	if(session) {
		secure_memzero(session, sizeof(fastd_method_session_state_t));
		free(session);
	}
}

static bool method_encrypt(fastd_context_t *ctx, fastd_peer_t *peer UNUSED, fastd_method_session_state_t *session, fastd_buffer_t *out, fastd_buffer_t in) {
	fastd_buffer_pull_head(ctx, &in, crypto_secretbox_xsalsa20poly1305_ZEROBYTES);
	memset(in.data, 0, crypto_secretbox_xsalsa20poly1305_ZEROBYTES);

	*out = fastd_buffer_alloc(ctx, in.len, 0, 0);

	uint8_t nonce[crypto_secretbox_xsalsa20poly1305_NONCEBYTES];
	memcpy(nonce, session->common.send_nonce, COMMON_NONCEBYTES);
	memset(nonce+COMMON_NONCEBYTES, 0, crypto_secretbox_xsalsa20poly1305_NONCEBYTES-COMMON_NONCEBYTES);

	crypto_secretbox_xsalsa20poly1305(out->data, in.data, in.len, nonce, session->key);

	fastd_buffer_free(in);

	fastd_buffer_push_head(ctx, out, crypto_secretbox_xsalsa20poly1305_BOXZEROBYTES-COMMON_NONCEBYTES);
	memcpy(out->data, session->common.send_nonce, COMMON_NONCEBYTES);

	fastd_method_increment_nonce(&session->common);

	return true;
}

static bool method_decrypt(fastd_context_t *ctx, fastd_peer_t *peer, fastd_method_session_state_t *session, fastd_buffer_t *out, fastd_buffer_t in) {
	if (in.len < COMMON_NONCEBYTES)
		return false;

	if (!method_session_is_valid(ctx, session))
		return false;

	uint8_t nonce[crypto_secretbox_xsalsa20poly1305_NONCEBYTES];
	memcpy(nonce, in.data, COMMON_NONCEBYTES);
	memset(nonce+COMMON_NONCEBYTES, 0, crypto_secretbox_xsalsa20poly1305_NONCEBYTES-COMMON_NONCEBYTES);

	int64_t age;
	if (!fastd_method_is_nonce_valid(ctx, &session->common, nonce, &age))
		return false;

	fastd_buffer_pull_head(ctx, &in, crypto_secretbox_xsalsa20poly1305_BOXZEROBYTES-COMMON_NONCEBYTES);
	memset(in.data, 0, crypto_secretbox_xsalsa20poly1305_BOXZEROBYTES);

	*out = fastd_buffer_alloc(ctx, in.len, 0, 0);

	if (crypto_secretbox_xsalsa20poly1305_open(out->data, in.data, in.len, nonce, session->key) != 0) {
		fastd_buffer_free(*out);

		/* restore input buffer */
		fastd_buffer_push_head(ctx, &in, crypto_secretbox_xsalsa20poly1305_BOXZEROBYTES-COMMON_NONCEBYTES);
		memcpy(in.data, nonce, COMMON_NONCEBYTES);
		return false;
	}

	fastd_buffer_free(in);

	if (!fastd_method_reorder_check(ctx, peer, &session->common, nonce, age)) {
		fastd_buffer_free(*out);
		*out = fastd_buffer_alloc(ctx, crypto_secretbox_xsalsa20poly1305_ZEROBYTES, 0, 0);
	}

	fastd_buffer_push_head(ctx, out, crypto_secretbox_xsalsa20poly1305_ZEROBYTES);

	return true;
}

const fastd_method_t fastd_method_xsalsa20_poly1305 = {
	.provides = method_provides,

	.max_packet_size = method_max_packet_size,
	.min_encrypt_head_space = method_min_encrypt_head_space,
	.min_decrypt_head_space = method_min_decrypt_head_space,
	.min_encrypt_tail_space = method_min_tail_space,
	.min_decrypt_tail_space = method_min_tail_space,

	.key_length = method_key_length,
	.session_init = method_session_init,
	.session_init_compat = method_session_init_compat,
	.session_is_valid = method_session_is_valid,
	.session_is_initiator = method_session_is_initiator,
	.session_want_refresh = method_session_want_refresh,
	.session_superseded = method_session_superseded,
	.session_free = method_session_free,

	.encrypt = method_encrypt,
	.decrypt = method_decrypt,
};