// Share accelerated compression with the bundled library's digest and key-derivation APIs.
#include "hash_core.h"
#include "mbedtls/sha256.h"

// Preserve library-owned buffering, padding, and the selected digest's initial state.
extern "C" int mbedtls_internal_sha256_process(mbedtls_sha256_context *p_ctx, const unsigned char p_data[64]) {
	GDCrypto::sha256_block(p_ctx->MBEDTLS_PRIVATE(state), p_data);
	return 0;
}
