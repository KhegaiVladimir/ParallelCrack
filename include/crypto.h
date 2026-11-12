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

/* ------------------------------------------------------------------------- */
/* AES-256-CBC file encryption, used by the synthetic file-recovery feature.  */
/* This demonstrates that a password found by the search can actually recover  */
/* a file that was encrypted with it. It is a demo, not a secure scheme.       */
/* ------------------------------------------------------------------------- */

/* AES-256 uses a 32-byte key; CBC uses a 16-byte initialization vector (IV). */
#define CRYPTO_AES_KEY_LEN 32
#define CRYPTO_AES_IV_LEN  16

/*
 * Derive a 32-byte AES key from a password by hashing it with SHA-256.
 * This is deliberately simple: it just turns a password string into key bytes so
 * the file-recovery demo has something to encrypt with. It is not a password
 * hashing scheme and is not meant to protect anything real.
 */
void crypto_key_from_password(const char *password,
                              unsigned char out_key[CRYPTO_AES_KEY_LEN]);

/*
 * Encrypt / decrypt a whole file with AES-256-CBC.
 *
 * Both return 0 on success and -1 on failure (file I/O or an OpenSSL error).
 * Decryption fails if the key is wrong, because CBC padding will not validate --
 * which is exactly the behavior we want: the recovered password must be correct
 * for the file to come back. The IV must be CRYPTO_AES_IV_LEN bytes.
 */
int crypto_encrypt_file(const unsigned char key[CRYPTO_AES_KEY_LEN],
                        const unsigned char iv[CRYPTO_AES_IV_LEN],
                        const char *in_path, const char *out_path);

int crypto_decrypt_file(const unsigned char key[CRYPTO_AES_KEY_LEN],
                        const unsigned char iv[CRYPTO_AES_IV_LEN],
                        const char *in_path, const char *out_path);

#endif /* PARALLELCRACK_CRYPTO_H */
