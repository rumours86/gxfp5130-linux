#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gxfp/proto/goodix_cmd.h"

#define TLS_REC_HDR_LEN 5
#define TLS_REC_MAX_PLAIN (16u * 1024u)

#define GXFP5130_PSK_LEN 32u
#define GXFP5130_PSK_IDENTITY "Client_identity"

/* GXFP51B7 uses a 48-byte PSK; 5130 provisioning still writes 32 bytes. */
#define GXFP51B7_PSK_LEN 48u

static inline int gxfp_tls_psk_length_valid(size_t len)
{
	return len == GXFP5130_PSK_LEN || len == GXFP51B7_PSK_LEN;
}
