/*
 * Copyright (C) 2013 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 */

#include "bid_private.h"

#ifdef __APPLE__
#include <pwd.h>
#include <sys/stat.h>
#endif

/*
 * Fast reauthentication support
 */

BIDError
_BIDAcquireDefaultTicketCache(BIDContext context)
{
    BIDError err;
    char szFileName[PATH_MAX];

#ifdef __APPLE__
    struct passwd *pw, pwd;
    char pwbuf[BUFSIZ];
    struct stat sb;

    if (getpwuid_r(geteuid(), &pwd, pwbuf, sizeof(pwbuf), &pw) < 0 ||
        pw == NULL ||
        pw->pw_dir == NULL) {
        err = BID_S_CACHE_OPEN_ERROR;
        goto cleanup;
    }

    snprintf(szFileName, sizeof(szFileName), "%s/Library/Caches/com.padl.gss.BrowserID", pw->pw_dir);

    if (stat(szFileName, &sb) < 0)
        mkdir(szFileName, 0700);

    snprintf(szFileName, sizeof(szFileName), "%s/Library/Caches/com.padl.gss.BrowserID/browserid.tickets.json", pw->pw_dir);
#else
    snprintf(szFileName, sizeof(szFileName), "/tmp/.browserid.tickets.%d.json", geteuid());
#endif

    err = _BIDAcquireCache(context, szFileName, &context->TicketCache);
    BID_BAIL_ON_ERROR(err);

cleanup:
    return err;
}

BIDError
_BIDStoreTicketInCache(
    BIDContext context,
    BIDIdentity identity,
    const char *szAudienceOrSpn,
    json_t *ticket)
{
    BIDError err;
    json_t *cred = NULL;
    BIDJWK ark = NULL;
    const char *szSubject = NULL;
    char *szCacheKey = NULL;

    BID_CONTEXT_VALIDATE(context);

    if (identity == BID_C_NO_IDENTITY ||
        szAudienceOrSpn == NULL ||
        ticket == NULL) {
        err = BID_S_INVALID_PARAMETER;
        goto cleanup;
    }

    if (context->TicketCache == NULL) {
        err = BID_S_NO_TICKET_CACHE;
        goto cleanup;
    }

    err = _BIDDeriveAuthenticatorRootKey(context, identity, &ark);
    BID_BAIL_ON_ERROR(err);

    cred = json_copy(identity->Attributes);
    if (cred == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    err = _BIDJsonObjectSet(context, cred, "tkt", ticket, BID_JSON_FLAG_REQUIRED);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectSet(context, cred, "ark", ark, BID_JSON_FLAG_REQUIRED);
    BID_BAIL_ON_ERROR(err);

    err = BIDGetIdentitySubject(context, identity, &szSubject);
    BID_BAIL_ON_ERROR(err);

    err = _BIDMakeAudience(context, szAudienceOrSpn, &szCacheKey);
    BID_BAIL_ON_ERROR(err);

    err = _BIDSetCacheObject(context, context->TicketCache, szCacheKey, cred);
    BID_BAIL_ON_ERROR(err);

cleanup:
    json_decref(cred);
    json_decref(ark);
    BIDFree(szCacheKey);

    return err;
}

BIDError
BIDStoreTicketInCache(
    BIDContext context,
    BIDIdentity identity,
    const char *szAudienceOrSpn,
    const char *szTicket)
{
    json_t *ticket;
    BIDError err;

    ticket = json_loads(szTicket, 0, &context->JsonError);
    if (ticket == NULL)
        return BID_S_INVALID_JSON;

    err = _BIDStoreTicketInCache(context, identity, szAudienceOrSpn, ticket);

    json_decref(ticket);

    return err;
}

static BIDError
_BIDMakeAuthenticator(
    BIDContext context,
    const char *szAudienceOrSpn,
    const unsigned char *pbChannelBindings,
    size_t cbChannelBindings,
    json_t *tkt,
    BIDJWT *pAuthenticator)
{
    BIDError err;
    BIDJWT ap;
    json_t *n = NULL;
    json_t *iat = NULL;
    json_t *exp = NULL;
    json_t *aud = NULL;
    json_t *cbt = NULL;

    *pAuthenticator = NULL;

    BID_CONTEXT_VALIDATE(context);

    if (tkt == NULL) {
        err = BID_S_BAD_TICKET_CACHE;
        goto cleanup;
    }

    err = _BIDGetCurrentJsonTimestamp(context, &iat);
    BID_BAIL_ON_ERROR(err);

    exp = json_integer(json_integer_value(iat) + context->Skew);
    if (exp == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    err = _BIDGenerateNonce(context, &n);
    BID_BAIL_ON_ERROR(err);

    aud = json_string(szAudienceOrSpn);
    if (aud == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    if (pbChannelBindings != NULL) {
        err = _BIDJsonBinaryValue(context, pbChannelBindings, cbChannelBindings, &cbt);
        BID_BAIL_ON_ERROR(err);
    }

    ap = BIDCalloc(1, sizeof(*ap));
    if (ap == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    ap->Payload = json_object();
    if (ap->Payload == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    err = _BIDJsonObjectSet(context, ap->Payload, "iat", iat, BID_JSON_FLAG_REQUIRED);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectSet(context, ap->Payload, "n", n, BID_JSON_FLAG_REQUIRED);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectSet(context, ap->Payload, "tkt", tkt, BID_JSON_FLAG_REQUIRED);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectSet(context, ap->Payload, "aud", aud, BID_JSON_FLAG_REQUIRED);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectSet(context, ap->Payload, "cbt", cbt, 0);
    BID_BAIL_ON_ERROR(err);

#ifdef GSSBID_DEBUG
    json_dumpf(ap->Payload, stdout, JSON_INDENT(8));
#endif

    *pAuthenticator = ap;

cleanup:
    if (err != BID_S_OK)
        _BIDReleaseJWT(context, ap);
    json_decref(iat);
    json_decref(exp);
    json_decref(n);
    json_decref(aud);
    json_decref(cbt);

    return err;
}

static BIDError
_BIDMakeReauthIdentity(
    BIDContext context,
    json_t *cred,
    BIDJWT ap,
    BIDIdentity *pIdentity)
{
    BIDError err;
    BIDIdentity identity = BID_C_NO_IDENTITY;
    json_t *credCopy = NULL;

    *pIdentity = NULL;

    credCopy = json_copy(cred);
    if (credCopy == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    err = _BIDAllocIdentity(context, credCopy, &identity);
    BID_BAIL_ON_ERROR(err);

    /* remove the secret stuff from the attribute cache */
    err = _BIDJsonObjectDel(context, identity->Attributes, "ark", 0);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectDel(context, identity->Attributes, "a-exp", 0);
    BID_BAIL_ON_ERROR(err);

    /* copy over the assertion expiry time */
    err = _BIDJsonObjectSet(context, identity->PrivateAttributes, "a-exp",
                            json_object_get(cred, "a-exp"), 0);
    BID_BAIL_ON_ERROR(err);

    err = _BIDDeriveAuthenticatorSessionKey(context, json_object_get(cred, "ark"), ap,
                                            &identity->SessionKey, &identity->SessionKeyLength);
    BID_BAIL_ON_ERROR(err);

    *pIdentity = identity;

cleanup:
    if (err != BID_S_OK)
        BIDReleaseIdentity(context, identity);
    json_decref(credCopy);

    return err;
}

static BIDError
_BIDFindTicketInCache(
    BIDContext context,
    BIDTicketCache ticketCache,
    const char *szPackedAudience,
    const char *szIdentityName,
    json_t **pCred)
{
    BIDError err;
    json_t *cred = NULL;

    if (szIdentityName != NULL) {
        const char *cacheKey;
        json_t *cacheVal = NULL;

        for (err = _BIDGetFirstCacheObject(context, ticketCache, &cacheKey, &cacheVal);
             err == BID_S_OK;
             err = _BIDGetNextCacheObject(context, ticketCache, &cacheKey, &cacheVal)) {
            const char *szCacheAudience = json_string_value(json_object_get(cacheVal, "aud"));
            const char *szCacheIdentity = json_string_value(json_object_get(cacheVal, "sub"));

            if (szCacheAudience == NULL || szCacheIdentity == NULL)
                continue;

            if (strcmp(szCacheAudience, szPackedAudience) == 0 &&
                strcmp(szCacheIdentity, szIdentityName) == 0) {
                cred = cacheVal;
                break;
            }

            json_decref(cacheVal);
            cacheVal = NULL;
        }

        if (err == BID_S_NO_MORE_ITEMS)
            err = BID_S_CACHE_KEY_NOT_FOUND;
    } else
        err = _BIDGetCacheObject(context, ticketCache, szPackedAudience, &cred);
    BID_BAIL_ON_ERROR(err);

    BID_ASSERT(err == BID_S_OK || cred == NULL);

    err = BID_S_OK;
    *pCred = cred;

cleanup:
    if (err != BID_S_OK)
        json_decref(cred);

    return err;
}

/*
 * Try to make a reauthentication assertion.
 */
BIDError
_BIDGetReauthAssertion(
    BIDContext context,
    BIDTicketCache ticketCache,
    const char *szPackedAudience,
    const unsigned char *pbChannelBindings,
    size_t cbChannelBindings,
    const char *szIdentityName,
    char **pAssertion,
    BIDIdentity *pAssertedIdentity,
    time_t *ptExpiryTime)
{
    BIDError err;
    json_t *cred = NULL;
    json_t *tkt = NULL;
    BIDJWT ap = NULL;
    struct BIDBackedAssertionDesc backedAssertion = { 0 };
    time_t now = 0;

    BID_CONTEXT_VALIDATE(context);
    BID_ASSERT(context->ContextOptions & BID_CONTEXT_REAUTH);

    if (pAssertion != NULL)
        *pAssertion = NULL;
    if (pAssertedIdentity != NULL)
        *pAssertedIdentity = NULL;
    if (ptExpiryTime != NULL)
        *ptExpiryTime = 0;

    if (ticketCache == BID_C_NO_TICKET_CACHE)
        ticketCache = context->TicketCache;

    err = _BIDFindTicketInCache(context, ticketCache, szPackedAudience, szIdentityName, &cred);
    BID_BAIL_ON_ERROR(err);

    tkt = json_object_get(cred, "tkt");
    if (tkt == NULL) {
        err = BID_S_BAD_TICKET_CACHE;
        goto cleanup;
    }

    err = _BIDMakeAuthenticator(context, szPackedAudience, pbChannelBindings, cbChannelBindings,
                                json_object_get(tkt, "jti"), &ap);
    BID_BAIL_ON_ERROR(err);

    _BIDGetJsonTimestampValue(context, ap->Payload, "iat", &now);

    err = _BIDValidateExpiry(context, now, tkt);
    BID_BAIL_ON_ERROR(err);

    backedAssertion.Assertion = ap;
    backedAssertion.cCertificates = 0;

    if (pAssertion != NULL) {
        err = _BIDPackBackedAssertion(context, &backedAssertion, json_object_get(cred, "ark"), pAssertion);
        BID_BAIL_ON_ERROR(err);
    }

    if (pAssertedIdentity != NULL) {
        err = _BIDMakeReauthIdentity(context, cred, ap, pAssertedIdentity);
        BID_BAIL_ON_ERROR(err);
    }

    if (ptExpiryTime != NULL)
        _BIDGetJsonTimestampValue(context, tkt, "exp", ptExpiryTime);

cleanup:
    json_decref(cred);
    _BIDReleaseJWT(context, ap);

    return err;
}

BIDError
_BIDVerifyReauthAssertion(
    BIDContext context,
    BIDReplayCache replayCache,
    BIDBackedAssertion assertion,
    time_t verificationTime,
    BIDIdentity *pVerifiedIdentity,
    BIDJWK *pVerifierCred)
{
    BIDError err;
    BIDJWT ap = assertion->Assertion;
    const char *szTicket;
    json_t *cred = NULL;

    *pVerifiedIdentity = BID_C_NO_IDENTITY;
    *pVerifierCred = NULL;

    BID_CONTEXT_VALIDATE(context);

    BID_ASSERT(context->ContextOptions & BID_CONTEXT_REPLAY_CACHE);
    BID_ASSERT(context->ContextOptions & BID_CONTEXT_REAUTH);
    BID_ASSERT(assertion->cCertificates == 0);

    szTicket = json_string_value(json_object_get(ap->Payload, "tkt"));

    if (replayCache == BID_C_NO_REPLAY_CACHE)
        replayCache = context->ReplayCache;

    err = _BIDGetCacheObject(context, replayCache, szTicket, &cred);
    if (err == BID_S_CACHE_NOT_FOUND || err == BID_S_CACHE_KEY_NOT_FOUND)
        err = BID_S_INVALID_ASSERTION;
    BID_BAIL_ON_ERROR(err);

    /*
     * _BIDVerifyLocal will verify the authenticator expiry as it is in the
     * claims and is the moral equivalent of the assertion expiry. However,
     * we also need to verify the ticket is still valid. We need to create
     * new object as _BIDValidateExpiry expects the expiry time to be in
     * the exp attribute.
     */
    err = _BIDValidateExpiry(context, verificationTime, cred);
    BID_BAIL_ON_ERROR(err);

    /*
     * Authenticators MUST expire Skew minutes after they are issued.
     * Delete the expiry attribute, if present, to prevent the initiator
     * sending an authenticator that expires too far into the future.
     */
    err = _BIDJsonObjectDel(context, ap->Payload, "exp", 0);
    BID_BAIL_ON_ERROR(err);

    *pVerifierCred = json_incref(json_object_get(cred, "ark"));

    err = _BIDVerifySignature(context, ap, *pVerifierCred);
    BID_BAIL_ON_ERROR(err);

    err = _BIDMakeReauthIdentity(context, cred, ap, pVerifiedIdentity);
    BID_BAIL_ON_ERROR(err);

cleanup:
    json_decref(cred);

    return err;
}

BIDError
_BIDDeriveAuthenticatorRootKey(
    BIDContext context,
    BIDIdentity identity,
    BIDJWK *pArk)
{
    unsigned char *pbSubkey = NULL;
    size_t cbSubkey;
    BIDError err;
    BIDJWK ark = NULL;
    json_t *sk = NULL;
    unsigned char salt[3] = "ARK";

    *pArk = NULL;

    err = BIDGetIdentitySessionKey(context, identity, NULL, NULL);
    BID_BAIL_ON_ERROR(err);

    err = _BIDDeriveKey(context, identity->SessionKey, identity->SessionKeyLength,
                        salt, sizeof(salt), &pbSubkey, &cbSubkey);
    BID_BAIL_ON_ERROR(err);

    ark = json_object();
    if (ark == NULL) {
        err = BID_S_NO_MEMORY;
        goto cleanup;
    }

    err = _BIDJsonBinaryValue(context, pbSubkey, cbSubkey, &sk);
    BID_BAIL_ON_ERROR(err);

    err = _BIDJsonObjectSet(context, ark, "secret-key", sk, 0);
    BID_BAIL_ON_ERROR(err);

    *pArk = ark;
    err = BID_S_OK;

cleanup:
    if (pbSubkey != NULL) {
        memset(pbSubkey, 0, cbSubkey);
        BIDFree(pbSubkey);
    }
    json_decref(sk);
    if (err != BID_S_OK)
        json_decref(ark);

    return err;
}

BIDError
_BIDDeriveAuthenticatorSessionKey(
    BIDContext context,
    BIDJWK ark,
    BIDJWT ap,
    unsigned char **ppbSessionKey,
    size_t *pcbSessionKey)
{
    BIDError err;
    unsigned char *pbArk = NULL;
    size_t cbArk;

    *ppbSessionKey = NULL;
    *pcbSessionKey = 0;

    err = _BIDGetJsonBinaryValue(context, ark, "secret-key", &pbArk, &cbArk);
    BID_BAIL_ON_ERROR(err);

    err = _BIDDeriveKey(context, pbArk, cbArk,
                        (unsigned char *)ap->EncData, ap->EncDataLength,
                        ppbSessionKey, pcbSessionKey);
    BID_BAIL_ON_ERROR(err);

    err = BID_S_OK;

cleanup:
    if (pbArk != NULL) {
        memset(pbArk, 0, cbArk);
        BIDFree(pbArk);
    }

    return err;
}

BIDError
BIDAcquireTicketCache(
    BIDContext context,
    const char *szCacheName,
    BIDTicketCache *pCache)
{
    return _BIDAcquireCache(context, szCacheName, pCache);
}

BIDError
BIDReleaseTicketCache(
    BIDContext context,
    BIDTicketCache cache)
{
    return _BIDReleaseCache(context, cache);
}

