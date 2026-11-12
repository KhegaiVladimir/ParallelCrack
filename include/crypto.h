#ifndef PARALLELCRACK_CRYPTO_H
#define PARALLELCRACK_CRYPTO_H

#include <stddef.h>

/*
 * Thin wrappers around the OpenSSL primitives the project needs. Keeping all
 * OpenSSL usage behind this small interface means the rest of the code never has
 * to deal with EVP contexts directly, and the search engine can verify a
 * candidate with a single function call.
 *
 * This header declares the hashing used for candidate verification. The
 * file-encryption helpers (AES-256-CBC) are added alongside the file-recovery
 * feature.
 */

/* SHA-256 produces a 32-byte digest. */
#define CRYPTO_SHA256_LEN 32

/*
 * Compute the SHA-256 digest of a buffer.
 * This is the verification primitive for the search: a candidate "matches" when
 * the SHA-256 of its bytes equals the target digest. We use a hash (rather than,
 * say, comparing plaintext) because it mirrors how real keyspace searches work
 * while keeping the workload completely synthetic and self-contained.
 */
void crypto_sha256(const unsigned char *data, size_t len,
                   unsigned char out[CRYPTO_SHA256_LEN]);

#endif /* PARALLELCRACK_CRYPTO_H */
