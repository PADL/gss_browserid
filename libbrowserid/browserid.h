/*
 * Copyright (C) 2013 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 */

#ifndef _BROWSERID_H_
#define _BROWSERID_H_ 1

#include <inttypes.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BID_S_OK,
    BID_S_NO_CONTEXT,
    BID_S_NO_MEMORY,
    BID_S_NOT_IMPLEMENTED,
    BID_S_INVALID_PARAMETER,
    BID_S_INVALID_USAGE,
    BID_S_UNAVAILABLE,
    BID_S_UNKNOWN_JSON_KEY,
    BID_S_INVALID_JSON,
    BID_S_INVALID_BASE64,
    BID_S_INVALID_ASSERTION,
    BID_S_CANNOT_ENCODE_JSON,
    BID_S_CANNOT_ENCODE_BASE64,
    BID_S_TOO_MANY_CERTS,
    BID_S_UNTRUSTED_ISSUER,
    BID_S_INVALID_ISSUER,
    BID_S_MISSING_ISSUER,
    BID_S_MISSING_AUDIENCE,
    BID_S_BAD_AUDIENCE,
    BID_S_EXPIRED_ASSERTION,
    BID_S_EXPIRED_CERT,
    BID_S_INVALID_SIGNATURE,
    BID_S_MISSING_ALGORITHM,
    BID_S_UNKNOWN_ALGORITHM,
    BID_S_INVALID_KEY,
    BID_S_INVALID_KEYSET,
    BID_S_NO_KEY,
    BID_S_CRYPTO_ERROR,
    BID_S_HTTP_ERROR,
    BID_S_BUFFER_TOO_SMALL,
    BID_S_BUFFER_TOO_LONG,
    BID_S_REMOTE_VERIFY_FAILURE,
    BID_S_MISSING_PRINCIPAL,
    BID_S_UNKNOWN_PRINCIPAL_TYPE,
    BID_S_MISSING_CERT,
    BID_S_UNKNOWN_ATTRIBUTE,
    BID_S_MISSING_CHANNEL_BINDINGS,
    BID_S_CHANNEL_BINDINGS_MISMATCH,
    BID_S_NO_SESSION_KEY,
    BID_S_DOCUMENT_NOT_MODIFIED,
    BID_S_INTERACT_UNAVAILABLE,
    BID_S_INTERACT_FAILURE,
    BID_S_INVALID_AUDIENCE_URN,
    BID_S_INVALID_JSON_WEB_TOKEN,
    BID_S_UNKNOWN_ERROR_CODE,
} BIDError;

BIDError
BIDErrorToString(
    BIDError error,
    const char **pString);

typedef struct BIDContextDesc *BIDContext;

#define BID_C_NO_CONTEXT                ((BIDContext)0)
#define BID_C_NO_IDENTITY               ((BIDIdentity)0)

/*
 * Context is used by user-agent.
 */
#define BID_CONTEXT_USER_AGENT          0x00000001

/*
 * Context is used by relying party.
 */
#define BID_CONTEXT_RP                  0x00000002

/*
 * Context uses remote verification service.
 */
#define BID_CONTEXT_VERIFY_REMOTE       0x00000004

/*
 * Context uses persistent authority cache.
 */
#define BID_CONTEXT_PERSIST_AUTH_CACHE  0x00000008

/*
 * Context is for GSS, required for channel binding support.
 */
#define BID_CONTEXT_GSS                 0x00000010

/*
 * Context management.
 */
BIDError
BIDAcquireContext(uint32_t ulContextOptions, BIDContext *pContext);

BIDError
BIDReleaseContext(BIDContext context);

#define BID_PARAM_AUDIENCES             0x00000001
#define BID_PARAM_TRUSTED_CERTS         0x00000002
#define BID_PARAM_VERIFIER_URL          0x00000003
#define BID_PARAM_JSON_ERROR_INFO       0x00000004 /* debug only */
#define BID_PARAM_MAX_DELEGATIONS       0x00000005
#define BID_PARAM_SKEW                  0x00000006
#define BID_PARAM_CONTEXT_OPTIONS       0x00000007

BIDError
BIDSetContextParam(BIDContext context, uint32_t ulParam, void *value);

BIDError
BIDGetContextParam(BIDContext context, uint32_t ulParam, void **pValue);

struct BIDIdentityDesc;
typedef struct BIDIdentityDesc *BIDIdentity;

/*
 * User agent.
 */
BIDError
BIDAcquireAssertion(
    BIDContext context,
    const char *szAssertion,
    BIDIdentity *pAssertedIdentity,
    time_t *pExpiryTime);

BIDError
BIDAcquireAssertionInteractive(
    BIDContext context,
    const char *szAudienceOrSpn,
    const unsigned char *pbChannelBindings,
    size_t cbChannelBindings,
    char **pAssertion,
    BIDIdentity *pAssertedIdentity,
    time_t *pExpiryTime);

BIDError
BIDFreeAssertion(
    BIDContext context,
    char *assertion);


/*
 * Verifier.
 */

BIDError
BIDVerifyAssertion(
    BIDContext context,
    const char *szAssertion,
    const char *szAudienceOrSpn,
    const unsigned char *pbChannelBindings,
    size_t cbChannelBindings,
    time_t tVerificationTime,
    BIDIdentity *pVerifiedIdentity,
    time_t *pExpiryTime);

BIDError
BIDGetIdentityAudience(
    BIDContext context,
    BIDIdentity identity,
    const char **pValue);

BIDError
BIDGetIdentityEmail(
    BIDContext context,
    BIDIdentity identity,
    const char **pValue);

BIDError
BIDGetIdentityIssuer(
    BIDContext context,
    BIDIdentity identity,
    const char **pValue);

BIDError
BIDGetIdentityAttribute(
    BIDContext context,
    BIDIdentity identity,
    const char *attribute,
    const char **pValue);

#ifdef JANSSON_H
/* Caller frees reference; pass NULL to get root object. */
BIDError
BIDGetIdentityJsonObject(
    BIDContext context,
    BIDIdentity identity,
    const char *attribute,
    json_t **pJsonValue);
#endif

BIDError
BIDGetIdentitySessionKey(
    BIDContext context,
    BIDIdentity identity,
    unsigned char **ppbSessionKey,
    size_t *pcbSessionKey);

BIDError
BIDFreeIdentitySessionKey(
    BIDContext context,
    BIDIdentity identity,
    unsigned char *pbSessionKey,
    size_t cbSessionKey);

BIDError
BIDReleaseIdentity(
    BIDContext context,
    BIDIdentity identity);

#ifdef __cplusplus
}
#endif

#endif /* _BROWSERID_H_ */