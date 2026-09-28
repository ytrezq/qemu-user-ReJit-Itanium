/* SPDX-License-Identifier: MIT */
/*
 * Cryptographic vector operations for TCG
 *
 * crypto_vec computes one step of a hash in host code, instead of a
 * helper, where the host has instructions for it (x86 SHA-NI).  Front ends
 * must check tcg_can_emit_crypto() and use their helpers otherwise.
 * Operands and result are TCG_TYPE_V128 vectors of 32-bit lanes, lane 0
 * lowest; the semantics are those of the Arm instructions of the same
 * name (FEAT_SHA256).
 */

#ifndef TCG_CRYPTO_H
#define TCG_CRYPTO_H

typedef enum TCGCryptoOp {
    /* 4 SHA-256 rounds: a = abcd, b = efgh, c = message + constants */
    TCG_CRYPTO_SHA256H,     /* result: the new abcd */
    TCG_CRYPTO_SHA256H2,    /* a = efgh, b = abcd; result: the new efgh */
    /* message schedule: a = W[t-16..t-13], b = W[t-12..t-9] */
    TCG_CRYPTO_SHA256SU0,   /* a + sigma0(a1, a2, a3, b0); c unused */
    /* a: SU0 result, b = W[t-12..t-9], c = W[t-4..t-1] */
    TCG_CRYPTO_SHA256SU1,
} TCGCryptoOp;

#endif /* TCG_CRYPTO_H */
