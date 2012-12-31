/*
 * Copyright (C) 2013 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 */
/*
 * Portions Copyright (c) 2009-2011 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include "bid_private.h"

BIDError
_BIDDuplicateString(
    BIDContext context,
    const char *szSrc,
    char **szDst)
{
    size_t cbSrc;

    if (szSrc == NULL)
        return BID_S_INVALID_PARAMETER;

    cbSrc = strlen(szSrc) + 1;

    *szDst = BIDMalloc(cbSrc);
    if (*szDst == NULL)
        return BID_S_NO_MEMORY;

    memcpy(*szDst, szSrc, cbSrc);
    return BID_S_OK;
}

BIDError
_BIDEncodeJson(
    BIDContext context,
    json_t *jData,
    char **pEncodedJson,
    size_t *pEncodedJsonLen)
{
    BIDError err;
    char *szJson;
    size_t len;

    *pEncodedJson = NULL;

    szJson = json_dumps(jData, JSON_COMPACT);
    if (szJson == NULL)
        return BID_S_CANNOT_ENCODE_JSON;

    err = _BIDBase64UrlEncode((unsigned char *)szJson, strlen(szJson), pEncodedJson, &len);
    if (err != BID_S_OK) {
        BIDFree(szJson);
        return err;
    }

    *pEncodedJsonLen = (size_t)len;

    BIDFree(szJson);

    return BID_S_OK;
}

BIDError
_BIDDecodeJson(
    BIDContext context,
    const char *encodedJson,
    json_t **pjData)
{
    BIDError err;
    char *szJson = NULL;
    size_t cbJson;
    json_t *jData;

    *pjData = NULL;

    err = _BIDBase64UrlDecode(encodedJson, (unsigned char **)&szJson, &cbJson);
    if (err != BID_S_OK) {
        BIDFree(szJson);
        return err;
    }

    /* XXX check valid string first? */
    szJson[cbJson] = '\0';

    jData = json_loads(szJson, 0, &context->JsonError);
    if (jData == NULL) {
        BIDFree(szJson);
        return BID_S_INVALID_JSON;
    }

    BIDFree(szJson);
    *pjData = jData;

    return BID_S_OK;
}

BIDError
_BIDUnpackBackedAssertion(
    BIDContext context,
    const char *encodedJson,
    BIDBackedAssertion *pAssertion)
{
    BIDError err;
    char *tmp = NULL, *p;
    BIDBackedAssertion assertion = NULL;

    if (encodedJson == NULL) {
        err = BID_S_INVALID_ASSERTION;
        goto cleanup;
    }

    err = _BIDDuplicateString(context, encodedJson, &tmp);
    BID_BAIL_ON_ERROR(err);

    assertion = BIDCalloc(1, sizeof(*assertion));
    if (assertion == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    for (p = tmp; p != NULL; ) {
        char *q = strchr(p, '~');
        BIDJWT *pDst;

        if (q != NULL) {
            if (assertion->cCertificates >= BID_MAX_CERTS) {
                err = BID_S_TOO_MANY_CERTS;
                goto cleanup;
            }

            *q = '\0';
            q++;
            pDst = &assertion->rCertificates[assertion->cCertificates];
        } else {
            pDst = &assertion->Assertion;
        }

        err = _BIDParseJWT(context, p, pDst);
        BID_BAIL_ON_ERROR(err);

        if (*pDst != assertion->Assertion)
            assertion->cCertificates++;

        p = q;
    }

    if (assertion->Assertion == NULL ||
        assertion->cCertificates == 0) {
        err = BID_S_INVALID_ASSERTION;
        goto cleanup;
    }

    *pAssertion = assertion;

cleanup:
    if (err != BID_S_OK)
        _BIDReleaseBackedAssertion(context, assertion);
    BIDFree(tmp);

    return err;
}

BIDError
_BIDPackBackedAssertion(
    BIDContext context,
    BIDBackedAssertion assertion,
    BIDJWKSet keyset,
    char **pEncodedJson)
{
    BIDError err;
    char *szEncodedAssertion = NULL;
    size_t cchEncodedAssertion;
    char *szEncodedCerts[BID_MAX_CERTS] = { NULL };
    size_t cchEncodedCerts[BID_MAX_CERTS] = { 0 };
    size_t i;
    size_t totalLen;
    char *p;

    *pEncodedJson = NULL;

    BID_ASSERT(assertion != NULL);

    err = _BIDMakeSignature(context, assertion->Assertion, keyset, &szEncodedAssertion, &cchEncodedAssertion);
    BID_BAIL_ON_ERROR(err);

    cchEncodedAssertion += 1; /* ~ */

    for (i = 0; i < assertion->cCertificates; i++) {
        err = _BIDMakeSignature(context, assertion->rCertificates[i], keyset, &szEncodedCerts[i], &cchEncodedCerts[i]);
        BID_BAIL_ON_ERROR(err);

        cchEncodedCerts[i] += 1; /* ~ */
    }

    totalLen = cchEncodedAssertion;
    for (i = 0; i < assertion->cCertificates && cchEncodedCerts[i] != 0; i++)
        totalLen += cchEncodedCerts[i];

    *pEncodedJson = BIDMalloc(totalLen + 1);
    if (*pEncodedJson == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    p = *pEncodedJson;
    *p++ = '~';
    memcpy(p, szEncodedAssertion, cchEncodedAssertion);
    p += cchEncodedAssertion;
    for (i = 0; i < assertion->cCertificates && cchEncodedCerts[i] != 0; i++) {
        *p++ = '~';
        memcpy(p, szEncodedCerts[i], cchEncodedCerts[i]);
    }
    *p = '\0';

    err = BID_S_OK;

cleanup:
    BIDFree(szEncodedAssertion);
    for (i = 0; i < assertion->cCertificates; i++)
        BIDFree(szEncodedCerts[i]);
    if (err != BID_S_OK)
        BIDFree(*pEncodedJson);

    return err;
}

BIDError
_BIDReleaseBackedAssertion(
    BIDContext context,
    BIDBackedAssertion assertion)
{
    size_t i;

    if (assertion == NULL)
        return BID_S_INVALID_PARAMETER;

    _BIDReleaseJWT(context, assertion->Assertion);
    for (i = 0; i < assertion->cCertificates; i++)
        _BIDReleaseJWT(context, assertion->rCertificates[i]);

    BIDFree(assertion);

    return BID_S_OK;
}

static BIDError
CURLcodeToBIDError(CURLcode cc)
{
    return (cc == CURLE_OK) ? BID_S_OK : BID_S_HTTP_ERROR;
}

static BIDError
_BIDSetCurlCompositeUrl(
    BIDContext context,
    CURL *curlHandle,
    const char *szHostname,
    const char *szRelativeUrl)
{
    char *szUrl;
    size_t cchHostname;
    size_t cchRelativeUrl;
    CURLcode cc;

    BID_ASSERT(szHostname != NULL);
    BID_ASSERT(szRelativeUrl != NULL);

    cchHostname = strlen(szHostname);
    cchRelativeUrl = strlen(szRelativeUrl);

    szUrl = BIDMalloc(sizeof("https://") + cchHostname + cchRelativeUrl);
    if (szUrl == NULL)
        return BID_S_NO_MEMORY;

    snprintf(szUrl, sizeof("https://") + cchHostname + cchRelativeUrl,
             "https://%s%s", szHostname, szRelativeUrl);

    cc = curl_easy_setopt(curlHandle, CURLOPT_URL, szUrl);

    BIDFree(szUrl);

    return CURLcodeToBIDError(cc);
}

struct BIDCurlBufferDesc {
    char *Data;
    size_t Offset;
    size_t Size;
};

static size_t
_BIDCurlWriteCB(void *ptr, size_t size, size_t nmemb, void *stream)
{
    struct BIDCurlBufferDesc *buffer = (struct BIDCurlBufferDesc *)stream;
    size_t sizeRequired;

    sizeRequired = buffer->Offset + (size * nmemb) + 1; /* NUL */

    if (sizeRequired > buffer->Size) {
        size_t newSize = buffer->Size;
        void *tmpBuffer;

        while (newSize < sizeRequired)
            newSize *= 2;

        tmpBuffer = BIDRealloc(buffer->Data, newSize);
        if (tmpBuffer == NULL)
            return 0;

        buffer->Data = tmpBuffer;
        buffer->Size = newSize;
    }

    memcpy(buffer->Data + buffer->Offset, ptr, size * nmemb);
    buffer->Offset += size * nmemb;

    return size * nmemb;
}

static BIDError
_BIDInitCurlHandle(
    BIDContext context,
    struct BIDCurlBufferDesc *buffer,
    CURL **pCurlHandle)
{
    CURLcode cc;
    CURL *curlHandle = NULL;
    char szUserAgent[64];

    *pCurlHandle = NULL;

    curlHandle = curl_easy_init();
    if (curlHandle == NULL)
        return BID_S_HTTP_ERROR;

    cc = curl_global_init(CURL_GLOBAL_SSL);
    BID_BAIL_ON_ERROR(cc);

    cc = curl_easy_setopt(curlHandle, CURLOPT_FOLLOWLOCATION, 1);
    BID_BAIL_ON_ERROR(cc);

#ifdef GSSBID_DEBUG
    cc = curl_easy_setopt(curlHandle, CURLOPT_VERBOSE, 1);
    BID_BAIL_ON_ERROR(cc);
#endif

    cc = curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, _BIDCurlWriteCB);
    BID_BAIL_ON_ERROR(cc);

    cc = curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, buffer);
    BID_BAIL_ON_ERROR(cc);

    cc = curl_easy_setopt(curlHandle, CURLOPT_FILETIME, 1);
    BID_BAIL_ON_ERROR(cc);

    snprintf(szUserAgent, sizeof(szUserAgent), "libbrowserid/%s", VERS_NUM);

    cc = curl_easy_setopt(curlHandle, CURLOPT_USERAGENT, szUserAgent);
    BID_BAIL_ON_ERROR(cc);

    *pCurlHandle = curlHandle;

cleanup:
    if (cc != CURLE_OK)
        curl_easy_cleanup(curlHandle);

    return CURLcodeToBIDError(cc);
}

#if 0
static BIDError
_BIDPopulateCacheMetadata(
    BIDContext context,
    struct BIDCurlBufferDesc *buffer,
    CURL *curlHandle,
    json_t *jsonDoc)
{
    CURLcode cc;

cleanup:
    return CURLcodeToBIDError(cc);
}
#endif

static BIDError
_BIDMakeHttpRequest(
    BIDContext context,
    struct BIDCurlBufferDesc *buffer,
    CURL *curlHandle,
    json_t **pJsonDoc)
{
    BIDError err;
    long httpStatus;

    *pJsonDoc = NULL;

    BID_ASSERT(buffer->Data != NULL);

    err = CURLcodeToBIDError(curl_easy_perform(curlHandle));
    BID_BAIL_ON_ERROR(err);

    buffer->Data[buffer->Offset] = '\0';

    err = CURLcodeToBIDError(curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpStatus));
    BID_BAIL_ON_ERROR(err);

    switch (httpStatus) {
    case 304:
        err = BID_S_DOCUMENT_NOT_MODIFIED;
        goto cleanup;
    case 200:
        break;
    default:
        err = BID_S_HTTP_ERROR;
    }

    *pJsonDoc = json_loads(buffer->Data, 0, &context->JsonError);
    if (*pJsonDoc == NULL) {
        err = BID_S_INVALID_JSON;
        goto cleanup;
    }

cleanup:
    return err;
}

static BIDError
_BIDSetCurlIfModifiedSince(
    BIDContext context,
    CURL *curlHandle,
    time_t tIfModifiedSince)
{
    CURLcode cc;
    long lIfModifiedSince = (long)tIfModifiedSince;
    curl_TimeCond timeCond;

    if (tIfModifiedSince <= 0)
        return BID_S_OK;

    timeCond = CURL_TIMECOND_IFMODSINCE;

    cc = curl_easy_setopt(curlHandle, CURLOPT_TIMECONDITION, timeCond);
    BID_BAIL_ON_ERROR(cc);

    cc = curl_easy_setopt(curlHandle, CURLOPT_TIMECONDITION, lIfModifiedSince);
    BID_BAIL_ON_ERROR(cc);

cleanup:
    return CURLcodeToBIDError(cc);
}

BIDError
_BIDRetrieveDocument(
    BIDContext context,
    const char *szHostname,
    const char *szRelativeUrl,
    time_t tIfModifiedSince,
    json_t **pJsonDoc)
{
    BIDError err;
    CURL *curlHandle = NULL;
    struct BIDCurlBufferDesc buffer = { NULL };

    *pJsonDoc = NULL;

    BID_CONTEXT_VALIDATE(context);

    buffer.Offset = 0;
    buffer.Size = BUFSIZ;
    buffer.Data = BIDMalloc(buffer.Size);
    if (buffer.Data == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    err = _BIDInitCurlHandle(context, &buffer, &curlHandle);
    BID_BAIL_ON_ERROR(err);

    err = _BIDSetCurlCompositeUrl(context, curlHandle, szHostname, szRelativeUrl);
    BID_BAIL_ON_ERROR(err);

    err = _BIDSetCurlIfModifiedSince(context, curlHandle, tIfModifiedSince);
    BID_BAIL_ON_ERROR(err);

    err = _BIDMakeHttpRequest(context, &buffer, curlHandle, pJsonDoc);
    BID_BAIL_ON_ERROR(err);

cleanup:
    curl_easy_cleanup(curlHandle);
    BIDFree(buffer.Data);

    return err;
}

BIDError
_BIDPostDocument(
    BIDContext context,
    const char *szUrl,
    const char *szPostFields,
    json_t **pJsonDoc)
{
    BIDError err;
    CURL *curlHandle = NULL;
    struct BIDCurlBufferDesc buffer = { NULL };

    *pJsonDoc = NULL;

    BID_CONTEXT_VALIDATE(context);

    buffer.Offset = 0;
    buffer.Size = BUFSIZ; /* XXX */
    buffer.Data = BIDMalloc(buffer.Size);
    if (buffer.Data == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    err = _BIDInitCurlHandle(context, &buffer, &curlHandle);
    BID_BAIL_ON_ERROR(err);

    err = CURLcodeToBIDError(curl_easy_setopt(curlHandle, CURLOPT_URL, szUrl));
    BID_BAIL_ON_ERROR(err);

    err = CURLcodeToBIDError(curl_easy_setopt(curlHandle, CURLOPT_POST, 1));
    BID_BAIL_ON_ERROR(err);

    err = CURLcodeToBIDError(curl_easy_setopt(curlHandle, CURLOPT_POSTFIELDS, szPostFields));
    BID_BAIL_ON_ERROR(err);

    err = _BIDMakeHttpRequest(context, &buffer, curlHandle, pJsonDoc);
    BID_BAIL_ON_ERROR(err);

cleanup:
    curl_easy_cleanup(curlHandle);
    BIDFree(buffer.Data);

    return err;
}

BIDError
_BIDGetJsonStringValue(
    BIDContext context,
    json_t *json,
    const char *key,
    char **pDst)
{
    const char *src = json_string_value(json_object_get(json, key));

    if (src == NULL)
        return BID_S_UNKNOWN_JSON_KEY;

    return _BIDDuplicateString(context, src, pDst);
}

BIDError
_BIDGetJsonBinaryValue(
    BIDContext context,
    json_t *json,
    const char *key,
    unsigned char **pbData,
    size_t *cbData)
{
    const char *src = json_string_value(json_object_get(json, key));

    if (src == NULL)
        return BID_S_UNKNOWN_JSON_KEY;

    return _BIDBase64UrlDecode(src, pbData, cbData);
}

const char *_BIDErrorTable[] = {
    "Success",
    "No context",
    "Out of memory",
    "Not implemented",
    "Invalid parameter",
    "Invalid usage",
    "Unavailable",
    "Unknown JSON key",
    "Invalid JSON",
    "Invalid Base64",
    "Invalid assertion",
    "Cannot encode JSON",
    "Cannot encode Base64",
    "Too many certs",
    "Untrusted issuer",
    "invalid issuer",
    "Missing issuer",
    "Missing audience",
    "Bad audience",
    "Expired assertion",
    "Expired certificate",
    "Invalid signature",
    "Missing algorithm",
    "Unknown algorithm",
    "Invalid key",
    "Invalid key set",
    "No key",
    "Internal crypto error",
    "HTTP error",
    "Buffer too small",
    "Buffer too large",
    "Remote verification failure",
    "Missing principal",
    "Unknown principal type",
    "Missing certificate",
    "Unknown attribute",
    "Missing channel bindings",
    "Channel bindings mismatch",
    "No session key",
    "Document not modified",
    "Process does not support UI interaction",
    "Failed to acquire assertion interactively",
    "Invalid audience URN",
    "Invalid JSON web token",
    "Unknown error code"
};

BIDError
BIDErrorToString(
    BIDError error,
    const char **pszErr)
{
    *pszErr = NULL;

    if (pszErr == NULL)
        return BID_S_INVALID_PARAMETER;

    if (error < BID_S_OK || error > BID_S_UNKNOWN_ERROR_CODE)
        return BID_S_UNKNOWN_ERROR_CODE;

    *pszErr = _BIDErrorTable[error];
    return BID_S_OK;
}

json_t *
_BIDLeafCert(
    BIDContext context,
    BIDBackedAssertion backedAssertion)
{
    if (backedAssertion->cCertificates == 0)
        return NULL;

    return backedAssertion->rCertificates[backedAssertion->cCertificates - 1]->Payload;
}

json_t *
_BIDRootCert(
    BIDContext context,
    BIDBackedAssertion backedAssertion)
{
    if (backedAssertion->cCertificates == 0)
        return NULL;

    return backedAssertion->rCertificates[0]->Payload;
}

BIDError
_BIDPopulateIdentity(
    BIDContext context,
    BIDBackedAssertion backedAssertion,
    BIDIdentity *pIdentity)
{
    BIDError err;
    BIDIdentity identity = NULL;
    json_t *assertion = backedAssertion->Assertion->Payload;
    json_t *leafCert = _BIDLeafCert(context, backedAssertion);
    json_t *principal;

    *pIdentity = NULL;

    identity = BIDCalloc(1, sizeof(*identity));
    if (identity == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    identity->Attributes = json_object();

    principal = json_object_get(leafCert, "principal");
    if (principal == NULL || json_object_get(principal, "email") == NULL) {
        err = BID_S_MISSING_PRINCIPAL;
        goto cleanup;
    }

    if (json_object_set(identity->Attributes, "email",    json_object_get(principal, "email")) ||
        json_object_set(identity->Attributes, "audience", json_object_get(assertion, "aud")) ||
        json_object_set(identity->Attributes, "issuer",   json_object_get(leafCert, "iss"))  ||
        json_object_set(identity->Attributes, "expires",  json_object_get(assertion, "exp"))) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    *pIdentity = identity;

cleanup:
    if (err != BID_S_OK)
        BIDReleaseIdentity(context, identity);

    return err;
}

BIDError
_BIDUnpackAudience(
    BIDContext context,
    const char *szPackedAudience,
    char **pszAudienceOrSpn,
    unsigned char **ppbChannelBindings,
    size_t *pcbChannelBindings)
{
    BIDError err;
    const char *p;
    char *szAudienceOrSpn = NULL;
    size_t cchAudienceOrSpn;
    unsigned char *pbChannelBindings = NULL;

    *pszAudienceOrSpn = NULL;
    *ppbChannelBindings = NULL;
    *pcbChannelBindings = 0;

    BID_CONTEXT_VALIDATE(context);

    if (szPackedAudience == NULL) {
        err = BID_S_INVALID_PARAMETER;
        goto cleanup;
    }

    if ((context->ContextOptions & BID_CONTEXT_GSS) == 0) {
        err = _BIDDuplicateString(context, szPackedAudience, pszAudienceOrSpn);
        BID_BAIL_ON_ERROR(err);

        err = BID_S_OK;
        goto cleanup;
    }

    cchAudienceOrSpn = strlen(szPackedAudience);

    if (cchAudienceOrSpn <= BID_GSS_AUDIENCE_PREFIX_LEN ||
        memcmp(szPackedAudience, BID_GSS_AUDIENCE_PREFIX, BID_GSS_AUDIENCE_PREFIX_LEN) != 0) {
        err = BID_S_INVALID_AUDIENCE_URN;
        goto cleanup;
    }

    szPackedAudience += BID_GSS_AUDIENCE_PREFIX_LEN;

#ifdef BROKEN_URL_PARSER
    p = strrchr(szPackedAudience, '.');
#else
    p = strrchr(szPackedAudience, '#');
#endif
    if (p != NULL) {
        if (p[1] != '\0') {
            err = _BIDBase64UrlDecode(p + 1, ppbChannelBindings, pcbChannelBindings);
            BID_BAIL_ON_ERROR(err);
        }

        cchAudienceOrSpn = p - szPackedAudience;
    } else {
        cchAudienceOrSpn = strlen(szPackedAudience);
    }

    szAudienceOrSpn = BIDMalloc(cchAudienceOrSpn + 1);
    if (szAudienceOrSpn == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    memcpy(szAudienceOrSpn, szPackedAudience, cchAudienceOrSpn);
    szAudienceOrSpn[cchAudienceOrSpn] = '\0';

#ifdef BROKEN_URL_PARSER
    p = strchr(szAudienceOrSpn, '.');
    if (p == NULL) {
        err = BID_S_INVALID_AUDIENCE_URN;
        goto cleanup;
    }
    *((char *)p) = '/';
#endif

    err = BID_S_OK;
    *pszAudienceOrSpn = szAudienceOrSpn;

cleanup:
    if (err != BID_S_OK) {
        BIDFree(szAudienceOrSpn);
        BIDFree(pbChannelBindings);
    }

    return err;
}

BIDError
_BIDPackAudience(
    BIDContext context,
    const char *szAudienceOrSpn,
    const unsigned char *pbChannelBindings,
    size_t cbChannelBindings,
    char **pszPackedAudience)
{
    BIDError err;
    char *szPackedAudience = NULL, *p;
    size_t cchAudienceOrSpn, cchPackedAudience;
    char *szEncodedChannelBindings = NULL;
    size_t cchEncodedChannelBindings;

    *pszPackedAudience = NULL;

    BID_CONTEXT_VALIDATE(context);

    if (szAudienceOrSpn == NULL) {
        err = BID_S_INVALID_PARAMETER;
        goto cleanup;
    }

    cchAudienceOrSpn = strlen(szAudienceOrSpn);

    if ((context->ContextOptions & BID_CONTEXT_GSS) == 0) {
        if (pbChannelBindings != NULL) {
            err = BID_S_INVALID_PARAMETER;
            goto cleanup;
        }

        err = _BIDDuplicateString(context, szAudienceOrSpn, pszPackedAudience);
        BID_BAIL_ON_ERROR(err);

        err = BID_S_OK;
        goto cleanup;
    }

    if (pbChannelBindings != NULL) {
        err = _BIDBase64UrlEncode(pbChannelBindings, cbChannelBindings, &szEncodedChannelBindings, &cchEncodedChannelBindings);
        BID_BAIL_ON_ERROR(err);
    } else {
        cchEncodedChannelBindings = 0;
    }

    cchPackedAudience = BID_GSS_AUDIENCE_PREFIX_LEN + cchAudienceOrSpn;
#ifdef BROKEN_URL_PARSER
        cchPackedAudience += 1 + cchEncodedChannelBindings;
#else
    if (cchEncodedChannelBindings != 0)
        cchPackedAudience += 1 + cchEncodedChannelBindings;
#endif

    szPackedAudience = BIDMalloc(cchPackedAudience + 1);
    if (szPackedAudience == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    p = szPackedAudience;
    memcpy(p, BID_GSS_AUDIENCE_PREFIX, BID_GSS_AUDIENCE_PREFIX_LEN);
    p += BID_GSS_AUDIENCE_PREFIX_LEN;
    memcpy(p, szAudienceOrSpn, cchAudienceOrSpn);
    p += cchAudienceOrSpn;
#ifdef BROKEN_URL_PARSER
    *p++ = '.';
#else
    if (cchEncodedChannelBindings != 0)
        *p++ = '#';
#endif
    memcpy(p, szEncodedChannelBindings, cchEncodedChannelBindings);
    p += cchEncodedChannelBindings;
    *p = '\0';

#ifdef BROKEN_URL_PARSER
    /* Convert the first / to a . */
    p = strchr(&szPackedAudience[BID_GSS_AUDIENCE_PREFIX_LEN], '/');
    if (p == NULL) {
        err = BID_S_BAD_AUDIENCE;
        goto cleanup;
    }
    *p = '.';
#endif

    err = BID_S_OK;
    *pszPackedAudience = szPackedAudience;

cleanup:
    if (err != BID_S_OK)
        BIDFree(szPackedAudience);
    BIDFree(szEncodedChannelBindings);

    return err;
}