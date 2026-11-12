#include "crypto.h"

#include <openssl/evp.h>

/*
 * Compute SHA-256 using the OpenSSL EVP ("envelope") interface.
 *
 * EVP is the modern, non-deprecated way to hash in OpenSSL 3. The older
 * SHA256_Init/Update/Final functions still exist but are deprecated, and the
 * even older DES helpers are deprecated as well; using EVP keeps the build warning
 * free and makes it easy to swap in a different hash later if we ever want to.
 */
void crypto_sha256(const unsigned char *data, size_t len,
                   unsigned char out[CRYPTO_SHA256_LEN])
{
    /* A message-digest context holds the in-progress hashing state. */
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (context == NULL) {
        /* Allocation failure is extremely unlikely here. There is no good way to
           recover mid-hash, so we zero the output to avoid returning stale stack
           bytes and return. Callers treat an all-zero digest as "no match". */
        for (int i = 0; i < CRYPTO_SHA256_LEN; i++) {
            out[i] = 0;
        }
        return;
    }

    /* Initialize the context for SHA-256, feed it the data, and read the digest. */
    EVP_DigestInit_ex(context, EVP_sha256(), NULL);
    EVP_DigestUpdate(context, data, len);
    EVP_DigestFinal_ex(context, out, NULL);

    EVP_MD_CTX_free(context);
}
