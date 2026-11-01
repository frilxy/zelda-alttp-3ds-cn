/*********************************************************************
* Filename:   sha256.h
* Author:     Brad Conte (brad AT bradconte.com)
* Source:     https://github.com/B-Con/crypto-algorithms
* Copyright:  Public domain (original disclaimer: presented "as is"
*             without any guarantees).
* Vendored:   2026 for zelda3randomizer (tasks.md §1.5). Modifications:
*               - Renamed BYTE/WORD typedefs to SHA256_BYTE/SHA256_WORD
*                 to avoid collision with src/types.h's BYTE(x)/WORD(x)
*                 cast macros.
*               - Switched <stddef.h> include to <stdint.h> + <stddef.h>.
*               - Added sha256_buffer() convenience wrapper.
* Details:    Defines the API for the corresponding SHA-256 implementation.
*********************************************************************/

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

/****************************** MACROS ******************************/
#define SHA256_BLOCK_SIZE 32            // SHA256 outputs a 32 byte digest

/**************************** DATA TYPES ****************************/
typedef uint8_t  SHA256_BYTE;
typedef uint32_t SHA256_WORD;

typedef struct {
	SHA256_BYTE data[64];
	SHA256_WORD datalen;
	unsigned long long bitlen;
	SHA256_WORD state[8];
} SHA256_CTX;

/*********************** FUNCTION DECLARATIONS **********************/
void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const SHA256_BYTE data[], size_t len);
void sha256_final(SHA256_CTX *ctx, SHA256_BYTE hash[]);

/* Convenience wrapper: hash a single contiguous buffer in one call. */
void sha256_buffer(const SHA256_BYTE *data, size_t len, SHA256_BYTE out_hash[SHA256_BLOCK_SIZE]);

#endif   // SHA256_H
