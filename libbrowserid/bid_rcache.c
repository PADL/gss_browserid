/*
 * Copyright (C) 2013 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 */

#include "bid_private.h"

BIDError
_BIDAcquireDefaultReplayCache(BIDContext context)
{
    BIDError err;

    err = _BIDAcquireCache(context, ".browserid.replay.json", &context->ReplayCache);
    BID_BAIL_ON_ERROR(err);

cleanup:
    return err;
}

BIDError
_BIDCheckReplayCache(
    BIDContext context,
    const char *szAssertion,
    time_t verificationTime)
{
    BIDError err;
    json_t *rdata;
    unsigned char hash[32];
    char *szHash = NULL;
    size_t cbHash = sizeof(hash), cchHash;
    time_t tsHash, expHash;

    err = _BIDDigestAssertion(context, szAssertion, hash, &cbHash);
    BID_BAIL_ON_ERROR(err);

    err = _BIDBase64UrlEncode(hash, cbHash, &szHash, &cchHash);
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetCacheObject(context, context->ReplayCache, szHash, &rdata);
    if (err == BID_S_OK) {
        _BIDGetJsonTimestampValue(context, rdata, "iat", &tsHash);
        _BIDGetJsonTimestampValue(context, rdata, "exp", &expHash);

        if (verificationTime < expHash)
            err = BID_S_REPLAYED_ASSERTION;
    } else
        err = BID_S_OK;

cleanup:
    BIDFree(szHash);
    json_decref(rdata);

    return err;
}

BIDError
_BIDUpdateReplayCache(
    BIDContext context,
    BIDIdentity identity,
    const char *szAssertion,
    time_t verificationTime)
{
    BIDError err;
    json_t *rdata;
    unsigned char hash[32];
    char *szHash = NULL;
    size_t cbHash = sizeof(hash), cchHash;
    json_t *expiryTime;
    json_t *ark = NULL;
    json_t *tkt = NULL;

    err = _BIDDigestAssertion(context, szAssertion, hash, &cbHash);
    BID_BAIL_ON_ERROR(err);

    err = _BIDBase64UrlEncode(hash, cbHash, &szHash, &cchHash);
    BID_BAIL_ON_ERROR(err);

    if (context->ContextOptions & BID_CONTEXT_REAUTH)
        rdata = json_copy(identity->Attributes);
    else
        rdata = json_object();
    if (rdata == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    err = _BIDSetJsonTimestampValue(context, rdata, "iat", verificationTime);
    BID_BAIL_ON_ERROR(err);

    expiryTime = json_object_get(identity->Attributes, "exp");
    if (expiryTime == NULL)
        _BIDSetJsonTimestampValue(context, rdata, "exp", verificationTime + 300);
    else
        json_object_set(rdata, "exp", expiryTime);

    if (context->ContextOptions & BID_CONTEXT_REAUTH) {
        err = _BIDDeriveAuthenticatorRootKey(context, identity, &ark);
        BID_BAIL_ON_ERROR(err);

        json_object_set(rdata, "ark", ark);
        _BIDSetJsonTimestampValue(context, rdata, "r-exp", verificationTime + context->TicketLifetime);
    }

    err = _BIDSetCacheObject(context, context->ReplayCache, szHash, rdata);
    BID_BAIL_ON_ERROR(err);

    BID_ASSERT(identity->PrivateAttributes != NULL);

    tkt = json_object();
    if (tkt == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    json_object_set_new(tkt, "jti", json_string(szHash));
    json_object_set(tkt, "exp", json_object_get(rdata, "r-exp"));
    json_object_set(identity->PrivateAttributes, "tkt", tkt);

cleanup:
    BIDFree(szHash);
    json_decref(ark);
    json_decref(rdata);

    return err;
}
