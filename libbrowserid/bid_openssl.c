/*
 * Copyright (c) 2013 PADL Software Pty Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Redistributions in any form must be accompanied by information on
 *    how to obtain complete source code for the libbrowserid software
 *    and any accompanying software that uses the libbrowserid software.
 *    The source code must either be included in the distribution or be
 *    available for no more than the cost of distribution plus a nominal
 *    fee, and must be freely redistributable under reasonable conditions.
 *    For an executable file, complete source code means the source code
 *    for all modules it contains. It does not include source code for
 *    modules or files that typically accompany the major components of
 *    the operating system on which the executable file runs.
 *
 * THIS SOFTWARE IS PROVIDED BY PADL SOFTWARE ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, OR
 * NON-INFRINGEMENT, ARE DISCLAIMED. IN NO EVENT SHALL PADL SOFTWARE
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "bid_private.h"

#include <openssl/obj_mac.h>
#include <openssl/rsa.h>
#include <openssl/dsa.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/dh.h>
#include <openssl/ecdh.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>

#include <ctype.h>

#ifdef GSSBID_DEBUG
#define BID_CRYPTO_PRINT_ERRORS() do { ERR_print_errors_fp(stderr); } while (0)
#else
#define BID_CRYPTO_PRINT_ERRORS()
#endif

#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

/*
 * Secret key agreement handle.
 */
struct BIDSecretHandleDesc {
    unsigned char *pbSecret;
    size_t cbSecret;
};
 
static BIDError
_BIDAllocSecret(
    BIDContext context,
    unsigned char *pbSecret,
    size_t cbSecret,
    int freeit,
    BIDSecretHandle *pSecretHandle);

static void
_BIDOpenSSLInit(void) __attribute__((__constructor__));

static void
_BIDOpenSSLInit(void)
{
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();
}

static BIDError
_BIDGetJsonBNValue(
    BIDContext context,
    BIDJWK jwk,
    const char *key,
    uint32_t encoding,
    BIGNUM **bn)
{
    BIDError err;
    json_t *value;
    const char *szValue;

    *bn = NULL;

    if (key != NULL)
        value = json_object_get(jwk, key);
    else
        value = jwk;
    if (value == NULL)
        return BID_S_NO_KEY;

    szValue = json_string_value(value);
    if (szValue == NULL)
        return BID_S_INVALID_KEY;

    err = BID_S_INVALID_KEY;

    if ((encoding == BID_ENCODING_BASE64_URL) ||
        !_BIDIsLegacyJWK(context, jwk)) {
        unsigned char buf[2048]; /* for large DH keys */
        unsigned char *pBuf = buf;
        size_t len = sizeof(buf);
        BIDError err2;

        err2 = _BIDBase64UrlDecode(szValue, &pBuf, &len);
        if (err2 == BID_S_OK) {
            *bn = BN_bin2bn(buf, (int)len, NULL);
            if (*bn != NULL)
                err = BID_S_OK;
            memset(buf, 0, sizeof(buf));
        }
    } else {
        size_t len = strlen(szValue), i;
        size_t cchDecimal = 0;

        /* XXX this is bogus, a hex string could also be a valid decimal string. */
        for (i = 0; i < len; i++) {
            if (isdigit(szValue[i]))
                cchDecimal++;
        }

        if (cchDecimal == len ? BN_dec2bn(bn, szValue) : BN_hex2bn(bn, szValue))
            err = BID_S_OK;
    }

    return err;
}

static BIDError
_BIDGetJsonECPointValue(
    BIDContext context,
    const EC_GROUP *group,
    json_t *json,
    EC_POINT **pEcPoint)
{
    BIDError err;
    BIGNUM *x = NULL;
    BIGNUM *y = NULL;
    EC_POINT *ecPoint = NULL;

    err = _BIDGetJsonBNValue(context, json, "x", BID_ENCODING_BASE64_URL, &x);
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetJsonBNValue(context, json, "y", BID_ENCODING_BASE64_URL, &y);
    BID_BAIL_ON_ERROR(err);

    ecPoint = EC_POINT_new(group);
    if (ecPoint == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    if (!EC_POINT_set_affine_coordinates_GFp(group, ecPoint, x, y, NULL)) {
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = BID_S_OK;
    *pEcPoint = ecPoint;

cleanup:
    if (err != BID_S_OK)
        EC_POINT_free(ecPoint);
    BN_free(x);
    BN_free(y);

    return err;
}

#if 0
static void
_BIDDebugJsonBNValue(
    BIDContext context,
    BIDJWK jwk,
    const char *key,
    uint32_t encoding)
{
    BIGNUM *bn;

    if (_BIDGetJsonBNValue(context, jwk, key, encoding, &bn) == BID_S_OK) {
        fprintf(stderr, "_BIDDebugJsonBNValue %s: ", key);
        BN_print_fp(stderr, bn);
        printf("\n");
        BN_free(bn);
    }
}
#endif

static BIDError
_BIDSetJsonBNValue(
    BIDContext context,
    BIDJWK jwk,
    const char *key,
    const BIGNUM *bn)
{
    BIDError err;
    unsigned char buf[1024];
    unsigned char *pbData;
    size_t cbData;
    int bFreeData = 0;
    json_t *j = NULL;

    cbData = BN_num_bytes(bn);
    if (cbData > sizeof(buf)) {
        pbData = BIDMalloc(cbData);
        if (pbData == NULL)
            return BID_S_NO_MEMORY;
        bFreeData = 1;
    } else
        pbData = buf;

    cbData = BN_bn2bin(bn, pbData);

    err = _BIDJsonBinaryValue(context, pbData, cbData, &j);
    if (err == BID_S_OK)
        err = _BIDJsonObjectSet(context, jwk, key, j, 0);

    if (bFreeData)
        BIDFree(pbData);
    json_decref(j);

    return err;
}

static BIDError
_BIDEvpForAlgorithmName(
    const char *szAlgID,
    const EVP_MD **pMd)
{
    const EVP_MD *md;

    *pMd = NULL;

    if (szAlgID == NULL || strlen(szAlgID) != 4)
        return BID_S_UNKNOWN_ALGORITHM;

    if (strcmp(szAlgID, "S128") == 0) {
        md = EVP_sha1();
    } else if (strcmp(szAlgID, "S512") == 0) {
        md = EVP_sha512();
    } else if (strcmp(szAlgID, "S384") == 0) {
        md = EVP_sha384();
    } else if (strcmp(szAlgID, "S256") == 0) {
        md = EVP_sha256();
    } else if (strcmp(szAlgID, "S224") == 0) {
        md = EVP_sha224();
    } else {
        return BID_S_UNKNOWN_ALGORITHM;
    }

    *pMd = md;
    return BID_S_OK;
}

static BIDError
_BIDEvpForAlgorithm(
    struct BIDJWTAlgorithmDesc *algorithm,
    const EVP_MD **pMd)
{
    *pMd = NULL;

    if (strlen(algorithm->szAlgID) != 5)
        return BID_S_CRYPTO_ERROR;

    return _BIDEvpForAlgorithmName(&algorithm->szAlgID[1], pMd);
}

static BIDError
_BIDMakeShaDigest(
    struct BIDJWTAlgorithmDesc *algorithm,
    BIDContext context BID_UNUSED,
    BIDJWT jwt,
    unsigned char *digest,
    size_t *digestLength)
{
    BIDError err;
    const EVP_MD *md;
    EVP_MD_CTX mdCtx;
    unsigned char shaMd[EVP_MAX_MD_SIZE] = { 0 };
    unsigned int mdLength = sizeof(md);

    err = _BIDEvpForAlgorithm(algorithm, &md);
    if (err != BID_S_OK)
        return err;

    if (*digestLength < EVP_MD_size(md))
        return BID_S_BUFFER_TOO_SMALL;

    EVP_DigestInit(&mdCtx, md);
    EVP_DigestUpdate(&mdCtx, jwt->EncData, jwt->EncDataLength);
    EVP_DigestFinal(&mdCtx, shaMd, &mdLength);

    if (*digestLength > mdLength)
        *digestLength = mdLength;

    memcpy(digest, shaMd, *digestLength);

    return BID_S_OK;
}

static BIDError
_BIDCertDataToX509(
    BIDContext context BID_UNUSED,
    json_t *x5c,
    int index,
    X509 **pX509)
{
    BIDError err;
    const char *szCert;
    unsigned char *pbData = NULL;
    const unsigned char *p;
    size_t cbData = 0;

    if (x5c == NULL) {
        err = BID_S_MISSING_CERT;
        goto cleanup;
    }

    szCert = json_string_value(json_array_get(x5c, index));
    if (szCert == NULL) {
        err = BID_S_MISSING_CERT;
        goto cleanup;
    }

    err = _BIDBase64UrlDecode(szCert, &pbData, &cbData);
    BID_BAIL_ON_ERROR(err);

    p = pbData;

    *pX509 = d2i_X509(NULL, &p, cbData);
    if (*pX509 == NULL) {
        err = BID_S_MISSING_CERT;
        goto cleanup;
    }

cleanup:
    BIDFree(pbData);

    return err;
}

static BIDError
_BIDCertDataToX509RsaKey(
    BIDContext context,
    json_t *x5c,
    RSA **pRsa)
{
    BIDError err;
    X509 *x509;
    EVP_PKEY *pkey;

    err = _BIDCertDataToX509(context, x5c, 0, &x509);
    if (err != BID_S_OK)
        return err;

    pkey = X509_get_pubkey(x509);
    if (pkey == NULL || EVP_PKEY_type(pkey->type) != EVP_PKEY_RSA) {
        X509_free(x509);
        return BID_S_NO_KEY;
    }

    RSA_up_ref(pkey->pkey.rsa);
    *pRsa = pkey->pkey.rsa;

    X509_free(x509);

    return BID_S_OK;
}

static BIDError
_BIDMakeJwtRsaKey(
    BIDContext context,
    BIDJWK jwk,
    int public,
    RSA **pRsa)
{
    BIDError err;
    RSA *rsa = NULL;

    rsa = RSA_new();
    if (rsa == NULL) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = _BIDGetJsonBNValue(context, jwk, "n", BID_ENCODING_UNKNOWN, &rsa->n);
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetJsonBNValue(context, jwk, "e", BID_ENCODING_UNKNOWN, &rsa->e);
    BID_BAIL_ON_ERROR(err);

    if (!public) {
        err = _BIDGetJsonBNValue(context, jwk, "d", BID_ENCODING_UNKNOWN, &rsa->d);
        BID_BAIL_ON_ERROR(err);
    }

    err = BID_S_OK;
    *pRsa = rsa;

cleanup:
    if (err != BID_S_OK)
        RSA_free(rsa);

    return err;
}

static BIDError
_BIDMakeRsaKey(
    BIDContext context,
    BIDJWK jwk,
    int public,
    RSA **pRsa)
{
    BIDError err;
    json_t *x5c;

    *pRsa = NULL;

    x5c = json_object_get(jwk, "x5c");
    if (public && x5c != NULL)
        err = _BIDCertDataToX509RsaKey(context, x5c, pRsa);
    else
        err = _BIDMakeJwtRsaKey(context, jwk, public, pRsa);

    return err;
}

static BIDError
_RSAKeySize(
    struct BIDJWTAlgorithmDesc *algorithm BID_UNUSED,
    BIDContext context,
    BIDJWK jwk,
    size_t *pcbKey)
{
    BIDError err;
    RSA *rsa = NULL;

    err = _BIDMakeRsaKey(context, jwk, 0, &rsa);
    if (err != BID_S_OK)
        return err;

    *pcbKey = RSA_size(rsa);
    RSA_free(rsa);

    return BID_S_OK;
}

static BIDError
_RSAMakeSignature(
    struct BIDJWTAlgorithmDesc *algorithm,
    BIDContext context,
    BIDJWT jwt,
    BIDJWK jwk)
{
    BIDError err;
    RSA *rsa = NULL;
    unsigned char digest[19 + EVP_MAX_MD_SIZE];
    size_t digestLength = EVP_MAX_MD_SIZE;
    ssize_t signatureLength;

    err = _BIDMakeRsaKey(context, jwk, 0, &rsa);
    BID_BAIL_ON_ERROR(err);

    BID_ASSERT(jwt->EncData != NULL);
    BID_ASSERT(algorithm->cbOid == sizeof(digest) - EVP_MAX_MD_SIZE);

    memcpy(digest, algorithm->pbOid, algorithm->cbOid);

    err = _BIDMakeShaDigest(algorithm, context, jwt, &digest[algorithm->cbOid], &digestLength);
    BID_BAIL_ON_ERROR(err);

    digestLength += algorithm->cbOid;

    jwt->Signature = BIDMalloc(RSA_size(rsa));
    if (jwt->Signature == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    signatureLength = RSA_private_encrypt((int)digestLength,
                                          digest,
                                          jwt->Signature,
                                          rsa,
                                          RSA_PKCS1_PADDING);

    if (signatureLength < 0) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    jwt->SignatureLength = signatureLength;
    err = BID_S_OK;

cleanup:
    RSA_free(rsa);

    return err;
}

static BIDError
_RSAVerifySignature(
    struct BIDJWTAlgorithmDesc *algorithm,
    BIDContext context,
    BIDJWT jwt,
    BIDJWK jwk,
    int *valid)
{
    BIDError err;
    RSA *rsa = NULL;
    unsigned char digest[19 + EVP_MAX_MD_SIZE];
    size_t digestLength = EVP_MAX_MD_SIZE;
    unsigned char *signature = NULL;
    ssize_t signatureLength;

    *valid = 0;

    err = _BIDMakeRsaKey(context, jwk, 1, &rsa);
    BID_BAIL_ON_ERROR(err);

    BID_ASSERT(jwt->EncData != NULL);
    BID_ASSERT(algorithm->cbOid == sizeof(digest) - EVP_MAX_MD_SIZE);

    memcpy(digest, algorithm->pbOid, algorithm->cbOid);

    err = _BIDMakeShaDigest(algorithm, context, jwt, &digest[algorithm->cbOid], &digestLength);
    BID_BAIL_ON_ERROR(err);

    digestLength += algorithm->cbOid;

    signature = BIDMalloc(RSA_size(rsa));
    if (signature == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    signatureLength = RSA_public_decrypt((int)jwt->SignatureLength,
                                         jwt->Signature,
                                         signature,
                                         rsa,
                                         RSA_PKCS1_PADDING);

    *valid = (signatureLength == digestLength &&
              _BIDTimingSafeCompare(signature, digest, signatureLength) == 0);

cleanup:
    RSA_free(rsa);
    BIDFree(signature);

    return err;
}

static BIDError
_BIDCertDataToX509DsaKey(
    BIDContext context,
    json_t *x5c,
    DSA **pDsa)
{
    BIDError err;
    X509 *x509;
    EVP_PKEY *pkey;

    err = _BIDCertDataToX509(context, x5c, 0, &x509);
    if (err != BID_S_OK)
        return err;

    pkey = X509_get_pubkey(x509);
    if (pkey == NULL || EVP_PKEY_type(pkey->type) != EVP_PKEY_RSA) {
        X509_free(x509);
        return BID_S_NO_KEY;
    }

    DSA_up_ref(pkey->pkey.dsa);
    *pDsa = pkey->pkey.dsa;

    X509_free(x509);

    return BID_S_OK;
}

static BIDError
_BIDMakeJwtDsaKey(BIDContext context, BIDJWK jwk, int public, DSA **pDsa)
{
    BIDError err;
    DSA *dsa = NULL;

    dsa = DSA_new();
    if (dsa == NULL) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = _BIDGetJsonBNValue(context, jwk, "p", BID_ENCODING_UNKNOWN, &dsa->p);
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetJsonBNValue(context, jwk, "q", BID_ENCODING_UNKNOWN, &dsa->q);
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetJsonBNValue(context, jwk, "g", BID_ENCODING_UNKNOWN, &dsa->g);
    BID_BAIL_ON_ERROR(err);

    if (public)
        err = _BIDGetJsonBNValue(context, jwk, "y", BID_ENCODING_UNKNOWN, &dsa->pub_key);
    else
        err = _BIDGetJsonBNValue(context, jwk, "x", BID_ENCODING_UNKNOWN, &dsa->priv_key);
    BID_BAIL_ON_ERROR(err);

    err = BID_S_OK;
    *pDsa = dsa;

cleanup:
    if (err != BID_S_OK)
        DSA_free(dsa);

    return err;
}

static BIDError
_BIDMakeDsaKey(
    BIDContext context,
    BIDJWK jwk,
    int public,
    DSA **pDsa)
{
    BIDError err;
    json_t *x5c;

    *pDsa = NULL;

    x5c = json_object_get(jwk, "x5c");
    if (public && x5c != NULL)
        err = _BIDCertDataToX509DsaKey(context, x5c, pDsa);
    else
        err = _BIDMakeJwtDsaKey(context, jwk, public, pDsa);

    return err;
}

static BIDError
_DSAKeySize(
    struct BIDJWTAlgorithmDesc *algorithm BID_UNUSED,
    BIDContext context,
    BIDJWK jwk,
    size_t *pcbKey)
{
    BIDError err;
    BIGNUM *p;
    size_t cbKey;

    err = _BIDGetJsonBNValue(context, jwk, "p", BID_ENCODING_UNKNOWN, &p);
    if (err != BID_S_OK)
        return err;

    /*
     * FIPS 186-3[3] specifies L and N length pairs of
     * (1024,160), (2048,224), (2048,256), and (3072,256).
     */
    cbKey = BN_num_bytes(p);
    if (cbKey < 160)
        cbKey = 160;
    else if (cbKey < 224)
        cbKey = 224;
    else if (cbKey < 256)
        cbKey = 256;

    BN_free(p);

    *pcbKey = cbKey;
    return BID_S_OK;
}

static BIDError
_DSAMakeSignature(
    struct BIDJWTAlgorithmDesc *algorithm,
    BIDContext context,
    BIDJWT jwt,
    BIDJWK jwk)
{
    BIDError err;
    DSA *dsa = NULL;
    DSA_SIG *dsaSig = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE];
    size_t digestLength = sizeof(digest);

    BID_ASSERT(jwt->EncData != NULL);

    err = _BIDMakeShaDigest(algorithm, context, jwt, digest, &digestLength);
    BID_BAIL_ON_ERROR(err);

    err = _BIDMakeDsaKey(context, jwk, 0, &dsa);
    BID_BAIL_ON_ERROR(err);

    dsaSig = DSA_do_sign(digest, (int)digestLength, dsa);
    if (dsaSig == NULL) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    if (BN_num_bytes(dsaSig->r) > digestLength ||
        BN_num_bytes(dsaSig->s) > digestLength) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    jwt->Signature = BIDMalloc(2 * digestLength);
    if (jwt->Signature == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    BN_bn2bin(dsaSig->r, &jwt->Signature[0]);
    BN_bn2bin(dsaSig->s, &jwt->Signature[digestLength]);

    jwt->SignatureLength = 2 * digestLength;

    err = BID_S_OK;

cleanup:
    DSA_free(dsa);
    DSA_SIG_free(dsaSig);

    return err;
}

static BIDError
_DSAVerifySignature(
    struct BIDJWTAlgorithmDesc *algorithm,
    BIDContext context,
    BIDJWT jwt,
    BIDJWK jwk,
    int *valid)
{
    BIDError err;
    DSA *dsa = NULL;
    DSA_SIG *dsaSig = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE];
    size_t digestLength = sizeof(digest);

    *valid = 0;

    BID_ASSERT(jwt->EncData != NULL);

    err = _BIDMakeDsaKey(context, jwk, 1, &dsa);
    BID_BAIL_ON_ERROR(err);

    err = _BIDMakeShaDigest(algorithm, context, jwt, digest, &digestLength);
    BID_BAIL_ON_ERROR(err);

    if (jwt->SignatureLength != 2 * digestLength) {
        err = BID_S_INVALID_SIGNATURE;
        goto cleanup;
    }

    dsaSig = DSA_SIG_new();
    if (dsaSig == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    dsaSig->r = BN_bin2bn(&jwt->Signature[0],            (int)digestLength, NULL);
    dsaSig->s = BN_bin2bn(&jwt->Signature[digestLength], (int)digestLength, NULL);

    *valid = DSA_do_verify(digest, (int)digestLength, dsaSig, dsa);
    if (*valid < 0) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = BID_S_OK;

cleanup:
    DSA_free(dsa);
    DSA_SIG_free(dsaSig);

    return err;
}

static BIDError
_BIDHMACSHA(
    struct BIDJWTAlgorithmDesc *algorithm,
    BIDContext context,
    BIDJWT jwt,
    BIDJWK jwk,
    unsigned char *hmac,
    size_t *hmacLength)
{
    BIDError err;
    HMAC_CTX h;
    const EVP_MD *md;
    unsigned char *pbKey = NULL;
    size_t cbKey = 0;
    unsigned int mdLen = (unsigned int)*hmacLength;

    BID_ASSERT(jwt->EncData != NULL);

    err = _BIDEvpForAlgorithm(algorithm, &md);
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetJsonBinaryValue(context, jwk, "secret-key", &pbKey, &cbKey);
    BID_BAIL_ON_ERROR(err);

    HMAC_Init(&h, pbKey, (int)cbKey, md);
    HMAC_Update(&h, (const unsigned char *)jwt->EncData, jwt->EncDataLength);
    HMAC_Final(&h, hmac, &mdLen);
    HMAC_cleanup(&h);

    *hmacLength = mdLen;

cleanup:
    if (pbKey != NULL) {
        memset(pbKey, 0, cbKey);
        BIDFree(pbKey);
    }

    return err;
}

static BIDError
_HMACSHAMakeSignature(
    struct BIDJWTAlgorithmDesc *algorithm,
    BIDContext context,
    BIDJWT jwt,
    BIDJWK jwk)
{
    BIDError err;
    unsigned char digest[EVP_MAX_MD_SIZE];
    size_t digestLength = sizeof(digest);

    BID_ASSERT(jwt->EncData != NULL);

    err = _BIDHMACSHA(algorithm, context, jwt, jwk, digest, &digestLength);
    BID_BAIL_ON_ERROR(err);

    jwt->Signature = BIDMalloc(digestLength);
    if (jwt->Signature == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    memcpy(jwt->Signature, digest, digestLength);
    jwt->SignatureLength = digestLength;

cleanup:
    memset(digest, 0, sizeof(digest));

    return err;
}

static BIDError
_HMACSHAVerifySignature(
    struct BIDJWTAlgorithmDesc *algorithm,
    BIDContext context,
    BIDJWT jwt,
    BIDJWK jwk,
    int *valid)
{
    BIDError err;
    unsigned char digest[EVP_MAX_MD_SIZE];
    size_t digestLength = sizeof(digest);

    BID_ASSERT(jwt->EncData != NULL);

    err = _BIDHMACSHA(algorithm, context, jwt, jwk, digest, &digestLength);
    if (err != BID_S_OK)
        return err;

    *valid = (jwt->SignatureLength == digestLength) &&
             (_BIDTimingSafeCompare(jwt->Signature, digest, digestLength) == 0);

    return BID_S_OK;
}

BIDError
_BIDMakeDigestInternal(
    BIDContext context,
    json_t *value,
    json_t *digestInfo)
{
    BIDError err;
    const char *szAlgID;
    const EVP_MD *md;
    EVP_MD_CTX mdCtx;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int mdLength = sizeof(digest);
    json_t *dig = NULL;

    szAlgID = json_string_value(json_object_get(digestInfo, "alg"));
    if (szAlgID == NULL) {
        err = BID_S_UNKNOWN_ALGORITHM;
        goto cleanup;
    }

    err = _BIDEvpForAlgorithmName(szAlgID, &md);
    BID_BAIL_ON_ERROR(err);

    BID_ASSERT(json_is_string(value));

    EVP_DigestInit(&mdCtx, md);
    EVP_DigestUpdate(&mdCtx, json_string_value(value), strlen(json_string_value(value)));
    EVP_DigestFinal(&mdCtx, digest, &mdLength);

    err = _BIDJsonBinaryValue(context, digest, mdLength, &dig);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectSet(context, digestInfo, "dig", dig, BID_JSON_FLAG_REQUIRED);
    BID_BAIL_ON_ERROR(err);

cleanup:
    json_decref(dig);

    return err;
}

struct BIDJWTAlgorithmDesc
_BIDJWTAlgorithms[] = {
#if 0
    {
        "RS512",
        "RSA",
        0,
        (const unsigned char *)"\x30\x51\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01\x05\x00\x04\x40",
        19,
        _RSAMakeSignature,
        _RSAVerifySignature,
        _RSAKeySize,
    },
    {
        "RS384",
        "RSA",
        0,
        (const unsigned char *)"\x30\x41\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01\x05\x00\x04\x30",
        19,
        _RSAMakeSignature,
        _RSAVerifySignature,
        _RSAKeySize,
    },
#endif
    {
        "RS256",
        "RSA",
        0,
        (const unsigned char *)"\x30\x31\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01\x05\x00\x04\x20",
        19,
        _RSAMakeSignature,
        _RSAVerifySignature,
        _RSAKeySize,
    },
    {
        "RS128",
        "RSA",
        0,
        (const unsigned char *)"\x30\x31\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01\x05\x00\x04\x20",
        19,
        _RSAMakeSignature,
        _RSAVerifySignature,
        _RSAKeySize,
    },
    {
        "RS64",
        "RSA",
        0,
        (const unsigned char *)"\x30\x31\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01\x05\x00\x04\x20",
        19,
        _RSAMakeSignature,
        _RSAVerifySignature,
        _RSAKeySize,
    },
    {
        "DS256",
        "DSA",
        256,
        NULL,
        0,
        _DSAMakeSignature,
        _DSAVerifySignature,
        _DSAKeySize,
    },
    {
        "DS128",
        "DSA",
        160,
        NULL,
        0,
        _DSAMakeSignature,
        _DSAVerifySignature,
        _DSAKeySize,
    },
    {
        "HS256",
        "HS",
        0,
        NULL,
        0,
        _HMACSHAMakeSignature,
        _HMACSHAVerifySignature,
        NULL,
    },
    {
        NULL
    },
};

BIDError
_BIDGenerateNonce(
    BIDContext context,
    json_t **pNonce)
{
    unsigned char nonce[16];

    *pNonce = NULL;

    if (!RAND_bytes(nonce, sizeof(nonce)))
        return BID_S_CRYPTO_ERROR;

    return _BIDJsonBinaryValue(context, nonce, sizeof(nonce), pNonce);
}

static const unsigned char _BIDSalt[9] = "BrowserID";

/*
 * Although this key derivation mechanism is a little unusual, it's designed
 * to be compatible with the Windows Cryptography Next Generation derivation
 * function, which does HMAC-Hash(Key, Prepend | Key | Append).
 */
BIDError
_BIDDeriveKey(
    BIDContext context BID_UNUSED,
    BIDSecretHandle secretHandle,
    const unsigned char *pbSalt,
    size_t cbSalt,
    unsigned char **ppbDerivedKey,
    size_t *pcbDerivedKey)
{
    HMAC_CTX h;
    unsigned char T1 = 0x01;
    unsigned int mdLength = SHA256_DIGEST_LENGTH;

    *ppbDerivedKey = NULL;
    *pcbDerivedKey = 0;

    if (secretHandle == NULL)
        return BID_S_INVALID_PARAMETER;

    if (secretHandle->pbSecret == NULL)
        return BID_S_INVALID_SECRET;

    *ppbDerivedKey = BIDMalloc(mdLength);
    if (*ppbDerivedKey == NULL)
        return BID_S_NO_MEMORY;

    HMAC_Init(&h, secretHandle->pbSecret, (int)secretHandle->cbSecret, EVP_sha256());
    HMAC_Update(&h, _BIDSalt, sizeof(_BIDSalt));
    HMAC_Update(&h, secretHandle->pbSecret, secretHandle->cbSecret);
    if (pbSalt != NULL)
        HMAC_Update(&h, pbSalt, cbSalt);
    HMAC_Update(&h, &T1, 1);

    HMAC_Final(&h, *ppbDerivedKey, &mdLength);
    HMAC_cleanup(&h);

    *pcbDerivedKey = mdLength;

    return BID_S_OK;
}

BIDError
_BIDLoadX509PrivateKey(
    BIDContext context BID_UNUSED,
    const char *path,
    const char *certPath BID_UNUSED,
    BIDJWK *pPrivateKey)
{
    BIDError err;
    BIDJWK privateKey = NULL;
    FILE *fp = NULL;
    EVP_PKEY *pemKey = NULL;

    *pPrivateKey = NULL;

    if (path == NULL) {
        err = BID_S_KEY_FILE_UNREADABLE;
        goto cleanup;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        err = BID_S_KEY_FILE_UNREADABLE;
        goto cleanup;
    }

    pemKey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    if (pemKey == NULL) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_KEY_FILE_UNREADABLE;
        goto cleanup;
    }

    err = _BIDAllocJsonObject(context, &privateKey);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectSet(context, privateKey, "version", json_string("2012.08.15"), BID_JSON_FLAG_CONSUME_REF);
    BID_BAIL_ON_ERROR(err);

    if (pemKey->pkey.ptr == NULL) {
        err = BID_S_INVALID_KEY;
        goto cleanup;
    }

    switch (pemKey->type) {
    case EVP_PKEY_RSA:
        err = _BIDJsonObjectSet(context, privateKey, "algorithm", json_string("RS"), BID_JSON_FLAG_CONSUME_REF);
        BID_BAIL_ON_ERROR(err);

        err = _BIDSetJsonBNValue(context, privateKey, "n", pemKey->pkey.rsa->n);
        BID_BAIL_ON_ERROR(err);

        err = _BIDSetJsonBNValue(context, privateKey, "e", pemKey->pkey.rsa->e);
        BID_BAIL_ON_ERROR(err);

        err = _BIDSetJsonBNValue(context, privateKey, "d", pemKey->pkey.rsa->d);
        BID_BAIL_ON_ERROR(err);

        break;
    case EVP_PKEY_DSA:
        err = _BIDJsonObjectSet(context, privateKey, "algorithm", json_string("DS"), BID_JSON_FLAG_CONSUME_REF);
        BID_BAIL_ON_ERROR(err);

        err = _BIDSetJsonBNValue(context, privateKey, "p", pemKey->pkey.dsa->p);
        BID_BAIL_ON_ERROR(err);

        err = _BIDSetJsonBNValue(context, privateKey, "q", pemKey->pkey.dsa->q);
        BID_BAIL_ON_ERROR(err);

        err = _BIDSetJsonBNValue(context, privateKey, "g", pemKey->pkey.dsa->g);
        BID_BAIL_ON_ERROR(err);

        err = _BIDSetJsonBNValue(context, privateKey, "x", pemKey->pkey.dsa->priv_key);
        BID_BAIL_ON_ERROR(err);

        break;
    default:
        err = BID_S_UNKNOWN_ALGORITHM;
        goto cleanup;
    }

    *pPrivateKey = privateKey;

cleanup:
    if (fp != NULL)
        fclose(fp);
    if (pemKey != NULL)
        EVP_PKEY_free(pemKey);
    if (err != BID_S_OK)
        json_decref(privateKey);

    return err;
}

BIDError
_BIDLoadX509Certificate(
    BIDContext context BID_UNUSED,
    const char *path,
    json_t **pCert)
{
    BIDError err;
    json_t *cert = NULL;
    FILE *fp = NULL;
    X509 *pemCert = NULL;
    unsigned char *pbData = NULL, *p;
    size_t cbData = 0;
    char *szData = NULL;

    *pCert = NULL;

    fp = fopen(path, "r");
    if (fp == NULL) {
        err = BID_S_CERT_FILE_UNREADABLE;
        goto cleanup;
    }

    pemCert = PEM_ASN1_read((void *(*) ()) d2i_X509, PEM_STRING_X509,
                            fp, NULL, NULL, NULL);
    if (pemCert == NULL) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CERT_FILE_UNREADABLE;
        goto cleanup;
    }

    cbData = i2d_X509(pemCert, NULL);

    p = pbData = BIDMalloc(cbData);
    if (pbData == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    if (i2d_X509(pemCert, &p) < 0) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = _BIDBase64Encode(pbData, cbData, BID_ENCODING_BASE64,
                           &szData, &cbData);
    BID_BAIL_ON_ERROR(err);

    cert = json_string(szData);
    if (cert == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    *pCert = cert;

cleanup:
    if (fp != NULL)
        fclose(fp);
    if (pemCert != NULL)
        X509_free(pemCert);
    if (err != BID_S_OK)
        json_decref(cert);
    BIDFree(pbData);
    BIDFree(szData);

    return err;
}

static BIDError
_BIDSetJsonX509CommonName(
    BIDContext context,
    json_t *j,
    const char *key,
    X509_NAME *name)
{
    char *szValue = NULL;
    BIDError err;
    int i;
    X509_NAME_ENTRY *cn;
    ASN1_STRING *cnValue;

    i = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
    if (i < 0)
        return BID_S_MISSING_PRINCIPAL;

    cn = X509_NAME_get_entry(name, i);
    if (cn == NULL)
        return BID_S_MISSING_PRINCIPAL;

    cnValue = X509_NAME_ENTRY_get_data(cn);
    ASN1_STRING_to_UTF8((unsigned char **)&szValue, cnValue);

    err = _BIDJsonObjectSet(context, j, key, json_string(szValue),
                            BID_JSON_FLAG_REQUIRED | BID_JSON_FLAG_CONSUME_REF);

    OPENSSL_free(szValue);

    return err;
}

static BIDError
_BIDSetJsonX509DN(
    BIDContext context,
    json_t *j,
    const char *key,
    X509_NAME *name)
{
    char szValue[BUFSIZ], *s;
    BIO *bio;
    long n;

    bio = BIO_new(BIO_s_mem());
    if (bio == NULL)
        return BID_S_NO_MEMORY;

    if (X509_NAME_print_ex(bio, name, 0, XN_FLAG_RFC2253) < 0)
        return BID_S_BAD_SUBJECT;

    n = BIO_get_mem_data(bio, &s);
    if (n >= sizeof(szValue))
        return BID_S_BUFFER_TOO_LONG;

    memcpy(szValue, s, n);
    szValue[n] = '\0';
    BIO_free(bio);

    return _BIDJsonObjectSet(context, j, key, json_string(szValue),
                             BID_JSON_FLAG_REQUIRED | BID_JSON_FLAG_CONSUME_REF);
}

static BIDError
_BIDSetJsonX509Name(
    BIDContext context,
    json_t *j,
    const char *key,
    X509_NAME *name,
    int cnOnly)
{
    if (cnOnly)
        return _BIDSetJsonX509CommonName(context, j, key, name);
    else
        return _BIDSetJsonX509DN(context, j, key, name);
}

static BIDError
_BIDSetJsonX509Time(
    BIDContext context,
    json_t *j,
    const char *key,
    ASN1_TIME *ts)
{
    struct tm tm = { 0 };
    const char *szTs = (const char *)ts->data;
    size_t cchTs = strlen(szTs), n;
    char zone;

    if (cchTs != 13 && cchTs != 15)
        return BID_S_INVALID_PARAMETER;

    if (cchTs == 13)
        n = sscanf(szTs, "%02d%02d%02d%02d%02d%02d%c",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
                   &zone);
    else
        n = sscanf(szTs, "%04d%02d%02d%02d%02d%02d%c",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
                   &zone);

    if (n != 7 || zone != 'Z')
        return BID_S_INVALID_PARAMETER;

    if (cchTs == 15)
        tm.tm_year -= 1900;
    else if (tm.tm_year < 90)
        tm.tm_year += 100;

    tm.tm_mon--;

    return _BIDSetJsonTimestampValue(context, j, key, timegm(&tm));
}

static BIDError
_BIDGetCertEKUs(
    BIDContext context BID_UNUSED,
    X509 *x509,
    json_t **pJsonEku)
{
    BIDError err;
    EXTENDED_KEY_USAGE *eku = NULL;
    int i;
    json_t *jsonEku = NULL;

    *pJsonEku = NULL;

    eku = X509_get_ext_d2i(x509, NID_ext_key_usage, NULL, NULL);
    if (eku == NULL) {
        err = BID_S_OK;
        goto cleanup;
    }

    jsonEku = json_array();
    if (jsonEku == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    for (i = 0; i < sk_ASN1_OBJECT_num(eku); i++) {
        ASN1_OBJECT *oid;
        char szOid[BUFSIZ];

        oid = sk_ASN1_OBJECT_value(eku, i);

        if (OBJ_obj2txt(szOid, sizeof(szOid), oid, 1) != -1)
            json_array_append_new(jsonEku, json_string(szOid));
    }

    *pJsonEku = jsonEku;
    err = BID_S_OK;

cleanup:
    if (eku != NULL)
        sk_ASN1_OBJECT_pop_free(eku, ASN1_OBJECT_free);
    if (err != BID_S_OK)
        json_decref(jsonEku);

    return err;
}

static BIDError
_BIDGetCertOtherName(
    BIDContext context,
    OTHERNAME *otherName,
    json_t **pJsonOtherName)
{
    BIDError err;
    json_t *jsonOtherName = NULL;
    ASN1_STRING *stringValue;
    char szOid[BUFSIZ];

    *pJsonOtherName = NULL;

    err = _BIDAllocJsonObject(context, &jsonOtherName);
    BID_BAIL_ON_ERROR(err);

    if (OBJ_obj2txt(szOid, sizeof(szOid), otherName->type_id, 1) < 0) {
        err = BID_S_INVALID_PARAMETER;
        goto cleanup;
    }

    err = _BIDJsonObjectSet(context, jsonOtherName, "oid",
                            json_string(szOid), BID_JSON_FLAG_CONSUME_REF);
    BID_BAIL_ON_ERROR(err);

    switch (otherName->value->type) {
    case V_ASN1_BMPSTRING:
    case V_ASN1_PRINTABLESTRING:
    case V_ASN1_IA5STRING:
    case V_ASN1_T61STRING:
    case V_ASN1_UTF8STRING:
    case V_ASN1_VISIBLESTRING:
    case V_ASN1_UNIVERSALSTRING:
    case V_ASN1_GENERALSTRING:
    case V_ASN1_NUMERICSTRING:
        stringValue = otherName->value->value.asn1_string;

        if (stringValue->data == NULL) {
            err = BID_S_INVALID_PARAMETER;
            goto cleanup;
        }

        err = _BIDJsonObjectSet(context, jsonOtherName, "value",
                                json_string((char *)stringValue->data),
                                BID_JSON_FLAG_CONSUME_REF);
        BID_BAIL_ON_ERROR(err);
        break;
    default:
        err = BID_S_INVALID_PARAMETER;
        goto cleanup;
        break;
    }

    *pJsonOtherName = jsonOtherName;

    err = BID_S_OK;

cleanup:
    if (err != BID_S_OK)
        json_decref(jsonOtherName);

    return err;
}

BIDError
_BIDPopulateX509Identity(
    BIDContext context,
    BIDBackedAssertion backedAssertion,
    BIDIdentity identity,
    uint32_t ulReqFlags)
{
    BIDError err;
    json_t *certChain = json_object_get(backedAssertion->Assertion->Header, "x5c");
    json_t *principal;
    json_t *eku = NULL;
    X509 *x509 = NULL;
    STACK_OF(GENERAL_NAME) *gens = NULL;
    int i;

    err = _BIDAllocJsonObject(context, &principal);
    BID_BAIL_ON_ERROR(err);

    err = _BIDCertDataToX509(context, certChain, 0, &x509);
    BID_BAIL_ON_ERROR(err);

    gens = X509_get_ext_d2i(x509, NID_subject_alt_name, NULL, NULL);
    if (gens != NULL) {
        for (i = 0; i < sk_GENERAL_NAME_num(gens); i++) {
            GENERAL_NAME *gen = sk_GENERAL_NAME_value(gens, i);
            const char *key = NULL;
            json_t *values = NULL;
            json_t *value = NULL;
            int bAttrExists = 0;

            switch (gen->type) {
            case GEN_OTHERNAME:
                key = "othername";
                break;
            case GEN_EMAIL:
                key = "email";
                break;
            case GEN_DNS:
                key = "hostname";
                break;
            case GEN_URI:
                key = "uri";
                break;
            default:
                continue;
                break;
            }

            if (strcmp(key, "othername") == 0) {
                err = _BIDGetCertOtherName(context, gen->d.otherName, &value);
                BID_BAIL_ON_ERROR(err);
            } else {
                value = json_string((char *)gen->d.ia5->data);
                if (value == NULL) {
                    err = BID_S_NO_MEMORY;
                    goto cleanup;
                }
            }

            values = json_object_get(principal, key);
            if (values == NULL)
                values = json_array();
            else
                bAttrExists = 1;

            json_array_append_new(values, value);

            if (!bAttrExists) {
                err = _BIDJsonObjectSet(context, principal, key, values,
                                        BID_JSON_FLAG_REQUIRED | BID_JSON_FLAG_CONSUME_REF);
                BID_BAIL_ON_ERROR(err);
            }
        }
    }

    err = _BIDJsonObjectSet(context, identity->Attributes, "principal", principal, 0);
    BID_BAIL_ON_ERROR(err);

    err = _BIDSetJsonX509Name(context, identity->Attributes, "sub", X509_get_subject_name(x509),
                              !!(ulReqFlags & BID_VERIFY_FLAG_RP));
    BID_BAIL_ON_ERROR(err);

    err = _BIDSetJsonX509Name(context, identity->Attributes, "iss", X509_get_issuer_name(x509), 0);
    BID_BAIL_ON_ERROR(err);

    err = _BIDSetJsonX509Time(context, identity->Attributes, "nbf", X509_get_notBefore(x509));
    BID_BAIL_ON_ERROR(err);

    err = _BIDSetJsonX509Time(context, identity->Attributes, "exp", X509_get_notAfter(x509));
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetCertEKUs(context, x509, &eku);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectSet(context, identity->Attributes, "eku", eku, 0);
    BID_BAIL_ON_ERROR(err);

cleanup:
    if (gens != NULL)
        sk_GENERAL_NAME_pop_free(gens, GENERAL_NAME_free);
    json_decref(principal);
    json_decref(eku);

    return err;
}

BIDError
_BIDValidateX509CertChain(
    BIDContext context,
    json_t *certChain,
    json_t *certParams,
    time_t verificationTime BID_UNUSED)
{
    BIDError err;
    X509_STORE *store = NULL;
    X509_STORE_CTX *storeCtx = NULL;
    X509 *leafCert = NULL;
    STACK_OF(X509) *chain = NULL;
    int i;
    json_t *caCertificateFile = NULL;
    json_t *caCertificateDir = NULL;

    if (json_array_size(certChain) == 0) {
        err = BID_S_MISSING_CERT;
        goto cleanup;
    }

    err = _BIDCertDataToX509(context, certChain, 0, &leafCert);
    BID_BAIL_ON_ERROR(err);

    chain = sk_X509_new_null();
    if (chain == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    for (i = 1; i < json_array_size(certChain); i++) {
        X509 *cert;

        err = _BIDCertDataToX509(context, certChain, i, &cert);
        BID_BAIL_ON_ERROR(err);

        sk_X509_push(chain, cert);
    }

    store = X509_STORE_new();
    storeCtx = X509_STORE_CTX_new();
    if (store == NULL || storeCtx == NULL) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    caCertificateFile = json_object_get(certParams, "ca-certificate");
    caCertificateDir = json_object_get(certParams, "ca-directory");

    if (X509_STORE_load_locations(store,
                                  json_string_value(caCertificateFile),
                                  json_string_value(caCertificateDir)) != 1 ||
        X509_STORE_set_default_paths(store) != 1 ||
        X509_STORE_CTX_init(storeCtx, store, leafCert, chain) != 1) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

#if 0
    X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
#endif

    if (!X509_verify_cert(storeCtx)) {
        BID_CRYPTO_PRINT_ERRORS();
        err = BID_S_UNTRUSTED_X509_CERT;
        goto cleanup;
    }

cleanup:
    if (chain != NULL)
        sk_X509_free(chain);
    if (storeCtx != NULL)
        X509_STORE_CTX_free(storeCtx);
    if (store != NULL)
        X509_STORE_free(store);

    return err;
}

BIDError
_BIDDestroySecret(
    BIDContext context BID_UNUSED,
    BIDSecretHandle secretHandle)
{
    if (secretHandle == NULL)
        return BID_S_INVALID_PARAMETER;

    if (secretHandle->pbSecret != NULL) {
        memset(secretHandle->pbSecret, 0, secretHandle->cbSecret);
        BIDFree(secretHandle->pbSecret);
    }

    memset(secretHandle, 0, sizeof(*secretHandle));
    BIDFree(secretHandle);

    return BID_S_OK;
}

static BIDError
_BIDAllocSecret(
    BIDContext context BID_UNUSED,
    unsigned char *pbSecret,
    size_t cbSecret,
    int freeit,
    BIDSecretHandle *pSecretHandle)
{
    BIDSecretHandle secretHandle;

    *pSecretHandle = NULL;

    secretHandle = BIDMalloc(sizeof(*secretHandle));
    if (secretHandle == NULL)
        return BID_S_NO_MEMORY;

    if (freeit) {
        secretHandle->pbSecret = pbSecret;
    } else {
        secretHandle->pbSecret = BIDMalloc(cbSecret);
        if (secretHandle->pbSecret == NULL) {
            BIDFree(secretHandle);
            return BID_S_NO_MEMORY;
        }

        memcpy(secretHandle->pbSecret, pbSecret, cbSecret);
    }

    secretHandle->cbSecret = cbSecret;

    *pSecretHandle = secretHandle;

    return BID_S_OK;
}

BIDError
_BIDImportSecretKeyData(
    BIDContext context,
    unsigned char *pbSecret,
    size_t cbSecret,
    BIDSecretHandle *pSecretHandle)
{
    return _BIDAllocSecret(context, pbSecret, cbSecret, 0, pSecretHandle);
}

static BIDError
_BIDMakeECKeyByCurve(
    BIDContext context BID_UNUSED,
    json_t *ecDhParams,
    EC_KEY **pEcKey)
{
    BIDError err;
    EC_KEY *ecKey = NULL;
    ssize_t curve = 0;
    int nid = 0;

    err = _BIDGetECDHCurve(context, ecDhParams, &curve);
    BID_BAIL_ON_ERROR(err);

    switch (curve) {
    case BID_CONTEXT_ECDH_CURVE_P256:
        nid = NID_X9_62_prime256v1;
        break;
    case BID_CONTEXT_ECDH_CURVE_P384:
        nid = NID_secp384r1;
        break;
    case BID_CONTEXT_ECDH_CURVE_P521:
        nid = NID_secp521r1;
        break;
    default:
        err = BID_S_UNKNOWN_EC_CURVE;
        goto cleanup;
        break;
    }

    ecKey = EC_KEY_new_by_curve_name(nid);
    if (ecKey == NULL) {
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = BID_S_OK;
    *pEcKey = ecKey;

cleanup:
    return err;
}

BIDError
_BIDGenerateECDHKey(
    BIDContext context,
    json_t *ecDhParams,
    BIDJWK *pEcDhKey)
{
    BIDError err;
    json_t *ecDhKey = NULL;
    EC_KEY *ec = NULL;
    BN_CTX *bnCtx = NULL;
    BIGNUM *x = NULL, *y = NULL;
    const EC_GROUP *group = NULL;
    const EC_POINT *publicKey = NULL;

    err = _BIDAllocJsonObject(context, &ecDhKey);
    BID_BAIL_ON_ERROR(err);

    err = _BIDMakeECKeyByCurve(context, ecDhParams, &ec);
    BID_BAIL_ON_ERROR(err);

    if (!EC_KEY_generate_key(ec)) {
        err = BID_S_DH_KEY_GENERATION_FAILURE;
        goto cleanup;
    }

    err = _BIDJsonObjectSet(context, ecDhKey, "params", ecDhParams, BID_JSON_FLAG_REQUIRED);
    BID_BAIL_ON_ERROR(err);

    bnCtx = BN_CTX_new();
    if (bnCtx == NULL) {
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    group = EC_KEY_get0_group(ec);
    publicKey = EC_KEY_get0_public_key(ec);

    BID_ASSERT(EC_METHOD_get_field_type(EC_GROUP_method_of(group)) == NID_X9_62_prime_field);

    x = BN_new();
    y = BN_new();
    if (x == NULL || y == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    if (!EC_POINT_get_affine_coordinates_GFp(group, publicKey, x, y, bnCtx)) {
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = _BIDSetJsonBNValue(context, ecDhKey, "x", x);
    BID_BAIL_ON_ERROR(err);

    err = _BIDSetJsonBNValue(context, ecDhKey, "y", y);
    BID_BAIL_ON_ERROR(err);

    err = _BIDSetJsonBNValue(context, ecDhKey, "d", EC_KEY_get0_private_key(ec));
    BID_BAIL_ON_ERROR(err);

    err = BID_S_OK;
    *pEcDhKey = ecDhKey;

cleanup:
    if (err != BID_S_OK) {
        json_decref(ecDhKey);
    }
    BN_free(x);
    BN_free(y);
    BN_CTX_free(bnCtx);
    EC_KEY_free(ec);

    return err;
}

static void *
_BIDKDFIdentity(
    const void *in,
    size_t inlen,
    void *out,
    size_t *outlen)
{
    if (*outlen < inlen)
        return NULL;

    memcpy(out, in, inlen);
    *outlen = inlen;

    return out;
}

BIDError
_BIDECDHSecretAgreement(
    BIDContext context,
    BIDJWK ecDhKey,
    json_t *pubValue,
    BIDSecretHandle *pSecretHandle)
{
    BIDError err;
    json_t *ecDhParams;
    unsigned char *pbKey = NULL;
    ssize_t cbKey = 0;
    EC_KEY *ec = NULL;
    const EC_GROUP *group = NULL;
    BIGNUM *d = NULL;
    EC_POINT *peerKey = NULL;
    EC_POINT *localKey = NULL;

    *pSecretHandle = NULL;

    if (ecDhKey == NULL || pubValue == NULL) {
        err = BID_S_INVALID_PARAMETER;
        goto cleanup;
    }

    ecDhParams = json_object_get(ecDhKey, "params");
    if (ecDhParams == NULL) {
        err = BID_S_INVALID_KEY;
        goto cleanup;
    }

    err = _BIDGetJsonBNValue(context, ecDhKey, "d", BID_ENCODING_BASE64_URL, &d);
    BID_BAIL_ON_ERROR(err);

    err = _BIDMakeECKeyByCurve(context, ecDhParams, &ec);
    BID_BAIL_ON_ERROR(err);

    group = EC_KEY_get0_group(ec);

    if (EC_KEY_set_private_key(ec, d) < 0) {
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = _BIDGetJsonECPointValue(context, group, ecDhKey, &localKey);
    BID_BAIL_ON_ERROR(err);

    if (EC_KEY_set_public_key(ec, localKey) < 0) {
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = _BIDGetJsonECPointValue(context, group, pubValue, &peerKey);
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetECDHCurve(context, ecDhParams, &cbKey);
    BID_BAIL_ON_ERROR(err);

    cbKey /= 8;
    cbKey++;

    pbKey = BIDMalloc(cbKey);
    if (pbKey == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    cbKey = ECDH_compute_key(pbKey, cbKey, peerKey, ec, _BIDKDFIdentity);
    if (cbKey < 0) {
        err = BID_S_CRYPTO_ERROR;
        goto cleanup;
    }

    err = _BIDAllocSecret(context, pbKey, cbKey, 1, pSecretHandle);
    BID_BAIL_ON_ERROR(err);

cleanup:
    if (err != BID_S_OK) {
        if (cbKey > 0)
            memset(pbKey, 0, cbKey);
        BIDFree(pbKey);
    }
    EC_POINT_free(peerKey);
    EC_POINT_free(localKey);
    EC_KEY_free(ec);
    BN_free(d);

    return err;
}

#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
