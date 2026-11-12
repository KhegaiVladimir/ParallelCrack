#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>

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

void crypto_key_from_password(const char *password,
                              unsigned char out_key[CRYPTO_AES_KEY_LEN])
{
    /* SHA-256 produces exactly 32 bytes, which is exactly an AES-256 key. */
    size_t length = 0;
    while (password[length] != '\0') {
        length++;
    }
    crypto_sha256((const unsigned char *)password, length, out_key);
}

/*
 * Read an entire file into a freshly allocated buffer. Returns the buffer (which
 * the caller must free) and writes its size to *out_size, or NULL on failure.
 * The files involved here are small synthetic test vectors, so reading them whole
 * keeps the code simple and easy to follow.
 */
static unsigned char *read_whole_file(const char *path, long *out_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);

    unsigned char *buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    size_t read_count = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_count != (size_t)size) {
        free(buffer);
        return NULL;
    }

    *out_size = size;
    return buffer;
}

/* Write a buffer to a file. Returns 0 on success, -1 on failure. */
static int write_whole_file(const char *path, const unsigned char *data, long size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    size_t written = fwrite(data, 1, (size_t)size, file);
    fclose(file);
    if (written != (size_t)size) {
        return -1;
    }
    return 0;
}

/*
 * Shared body for encrypt and decrypt. "encrypting" selects the direction. Using
 * one function for both keeps the OpenSSL bookkeeping in a single place; the only
 * things that differ are the init/update/final function names, which we select
 * with the flag.
 */
static int aes_cbc_transform(int encrypting,
                             const unsigned char key[CRYPTO_AES_KEY_LEN],
                             const unsigned char iv[CRYPTO_AES_IV_LEN],
                             const char *in_path, const char *out_path)
{
    long in_size = 0;
    unsigned char *input = read_whole_file(in_path, &in_size);
    if (input == NULL) {
        return -1;
    }

    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (context == NULL) {
        free(input);
        return -1;
    }

    /* The output can grow by at most one cipher block (16 bytes) due to padding. */
    unsigned char *output = malloc((size_t)in_size + CRYPTO_AES_IV_LEN);
    if (output == NULL) {
        EVP_CIPHER_CTX_free(context);
        free(input);
        return -1;
    }

    int result = -1;
    int out_length_1 = 0;
    int out_length_2 = 0;

    if (encrypting == 1) {
        if (EVP_EncryptInit_ex(context, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
            EVP_EncryptUpdate(context, output, &out_length_1, input, (int)in_size) == 1 &&
            EVP_EncryptFinal_ex(context, output + out_length_1, &out_length_2) == 1) {
            result = 0;
        }
    } else {
        /* DecryptFinal returns 0 if the padding is invalid, which is how a wrong
           key (wrong password) is detected. In that case result stays -1. */
        if (EVP_DecryptInit_ex(context, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
            EVP_DecryptUpdate(context, output, &out_length_1, input, (int)in_size) == 1 &&
            EVP_DecryptFinal_ex(context, output + out_length_1, &out_length_2) == 1) {
            result = 0;
        }
    }

    if (result == 0) {
        long total = (long)out_length_1 + (long)out_length_2;
        result = write_whole_file(out_path, output, total);
    }

    EVP_CIPHER_CTX_free(context);
    free(output);
    free(input);
    return result;
}

int crypto_encrypt_file(const unsigned char key[CRYPTO_AES_KEY_LEN],
                        const unsigned char iv[CRYPTO_AES_IV_LEN],
                        const char *in_path, const char *out_path)
{
    return aes_cbc_transform(1, key, iv, in_path, out_path);
}

int crypto_decrypt_file(const unsigned char key[CRYPTO_AES_KEY_LEN],
                        const unsigned char iv[CRYPTO_AES_IV_LEN],
                        const char *in_path, const char *out_path)
{
    return aes_cbc_transform(0, key, iv, in_path, out_path);
}
