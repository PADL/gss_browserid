/*
 * Copyright (C) 1813 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>
#include "browserid.h"
#include "bid_private.h"

static BIDContext gContext = NULL;
static time_t gNow = 0;

static void
BIDToolUsage(void);
static void
BIDAbortError(const char *szMessage, BIDError err);
static BIDError
BIDPurgeCache(int argc, char *argv[], BIDCache cache, int (*shouldPurgeP)(json_t *));

static BIDError
BIDPrintTicketCacheEntry(json_t *j)
{
    const char *szExpiry;
    time_t expiryTime;
 
    expiryTime = json_integer_value(json_object_get(j, "expires"));
 
    szExpiry = gNow < expiryTime ? ctime(&expiryTime) : ">>> Expired <<<";

    printf("%-15.15s %-25.25s %-18.18s %-20.20s\n",
           json_string_value(json_object_get(j, "email")),
           json_string_value(json_object_get(j, "audience")),
           json_string_value(json_object_get(j, "issuer")),
           szExpiry);
    printf("\n");

    return BID_S_OK;
}

static int
BIDShouldPurgeTicketCacheEntryP(json_t *j)
{
    time_t expiryTime;

    _BIDGetJsonTimestampValue(gContext, j, "exp", &expiryTime);

    return expiryTime == 0 || gNow >= expiryTime;
}

static BIDError
BIDPurgeTicketCache(int argc, char *argv[])
{
    return BIDPurgeCache(argc, argv, gContext->TicketCache, BIDShouldPurgeTicketCacheEntryP);
}

static BIDError
BIDListTicketCache(int argc, char *argv[])
{
    BIDError err;
    const char *k = NULL;
    json_t *j = NULL;
    int i;

    if (argc)
        BIDToolUsage();

    if (gContext->TicketCache == NULL)
        return BID_S_INVALID_PARAMETER;

    printf("%-15.15s %-25.25s %-18.18s %-20.20s\n",
           "Identity", "Audience", "Issuer", "Expires");
    for (i = 0; i < 80; i++)
        printf("-");
    printf("\n");

    for (err = _BIDGetFirstCacheObject(gContext, gContext->TicketCache, &k, &j);
         err == BID_S_OK;
         err = _BIDGetNextCacheObject(gContext, gContext->TicketCache, &k, &j)) {
        BIDPrintTicketCacheEntry(j);
        json_decref(j);
        j = NULL;
    }

    if (err == BID_S_NO_MORE_ITEMS)
        err = BID_S_OK;

    json_decref(j);
    return err;
}

static BIDError
BIDDestroyTicketCache(int argc, char *argv[])
{
    if (argc)
        BIDToolUsage();

    if (gContext->TicketCache == NULL)
        return BID_S_INVALID_PARAMETER;

    return _BIDDestroyCache(gContext, gContext->TicketCache);
}

static BIDError
BIDPrintReplayCacheEntry(const char *k, json_t *j)
{
    BIDError err;
    unsigned char *hash = NULL;
    size_t cbHash, i;
    time_t exp, ts;

    err = _BIDBase64UrlDecode(k, &hash, &cbHash);
    BID_BAIL_ON_ERROR(err);

    _BIDGetJsonTimestampValue(gContext, j, "ts", &ts);
    _BIDGetJsonTimestampValue(gContext, j, "exp", &exp);

    printf("%-24.24s  ", ctime(&ts));

    for (i = 0; i < cbHash; i++)
        printf("%02X", hash[i] & 0xff);

    printf("\n");

cleanup:
    BIDFree(hash);

    return err;
}

static int
BIDShouldPurgeReplayCacheEntryP(json_t *j)
{
    time_t expiryTime;

    _BIDGetJsonTimestampValue(gContext, j, "exp", &expiryTime);

    return expiryTime == 0 || gNow >= expiryTime;
}

static BIDError
BIDListReplayCache(int argc, char *argv[])
{
    BIDError err;
    const char *k = NULL;
    json_t *j = NULL;
    int i;

    if (argc)
        BIDToolUsage();

    if (gContext->ReplayCache == NULL)
        return BID_S_INVALID_PARAMETER;

    printf("%-24.24s  %s\n", "Timestamp", "Hash");
    for (i = 0; i < 90; i++)
        printf("-");
    printf("\n");

    for (err = _BIDGetFirstCacheObject(gContext, gContext->ReplayCache, &k, &j);
         err == BID_S_OK;
         err = _BIDGetNextCacheObject(gContext, gContext->ReplayCache, &k, &j)) {
        BIDPrintReplayCacheEntry(k, j);
        json_decref(j);
        j = NULL;
    }

    if (err == BID_S_NO_MORE_ITEMS)
        err = BID_S_OK;

    json_decref(j);
    return err;
}

static BIDError
BIDPurgeCache(int argc, char *argv[], BIDCache cache, int (*shouldPurgeP)(json_t *))
{
    BIDError err;
    const char *k = NULL;
    json_t *j = NULL;

    if (argc)
        BIDToolUsage();

    if (cache == NULL)
        return BID_S_INVALID_PARAMETER;

    for (err = _BIDGetFirstCacheObject(gContext, gContext->ReplayCache, &k, &j);
         err == BID_S_OK;
         err = _BIDGetNextCacheObject(gContext, gContext->ReplayCache, &k, &j)) {
        if (shouldPurgeP(j))
            _BIDRemoveCacheObject(gContext, cache, k);
    }

    if (err == BID_S_NO_MORE_ITEMS)
        err = BID_S_OK;

    json_decref(j);
    return err;
}

static BIDError
BIDPurgeReplayCache(int argc, char *argv[])
{
    return BIDPurgeCache(argc, argv, gContext->ReplayCache, BIDShouldPurgeReplayCacheEntryP);
}

static BIDError
BIDDestroyReplayCache(int argc, char *argv[])
{
    if (argc)
        BIDToolUsage();

    if (gContext->ReplayCache == NULL)
        return BID_S_INVALID_PARAMETER;

    return _BIDDestroyCache(gContext, gContext->ReplayCache);
}

static int
BIDShouldPurgeAuthorityP(json_t *j)
{
    time_t expiryTime = json_integer_value(json_object_get(j, "expires"));

    return expiryTime == 0 || gNow >= expiryTime;
}

static BIDError
BIDPrintAuthorityCacheEntry(const char *k, json_t *j)
{
    BIDError err;
    BIDBackedAssertion backedAssertion = NULL;
    BIDIdentity identity = NULL;
    BIDJWK publicKey = NULL;
    const char *szAlgorithm;
    time_t expiryTime;
    const char *szExpiry;

    expiryTime = json_integer_value(json_object_get(j, "expires"));

    err = _BIDGetAuthorityPublicKey(gContext, j, &publicKey);
    if (err == BID_S_OK) {
        json_t *p = json_object_get(publicKey, "public-key");

        szAlgorithm = json_string_value(json_object_get(p, "algorithm"));
        if (szAlgorithm == NULL)
            szAlgorithm = json_string_value(json_object_get(p, "alg"));

        if (strcmp(szAlgorithm, "RS") == 0)
            szAlgorithm = "RSA";
        else if (strcmp(szAlgorithm, "DS") == 0)
            szAlgorithm = "DSA";

        json_decref(publicKey);
    }

    if (szAlgorithm == NULL)
        szAlgorithm = "UNK";

    szExpiry = gNow < expiryTime ? ctime(&expiryTime) : ">>> Expired <<<";

    printf("%-30.30s %-4.4s %-20.20s\n",
           k, szAlgorithm, szExpiry);

    _BIDReleaseBackedAssertion(gContext, backedAssertion);
    BIDReleaseIdentity(gContext, identity);

    return err;
}

static BIDError
BIDListAuthorityCache(int argc, char *argv[])
{
    BIDError err;
    const char *k = NULL;
    json_t *j = NULL;
    int i;

    if (argc)
        BIDToolUsage();

    if (gContext->AuthorityCache == NULL)
        return BID_S_INVALID_PARAMETER;

    printf("%-30.30s %-4.4s %-20.20s\n", "Issuer", "ALG", "Expires");
    for (i = 0; i < 60; i++)
        printf("-");
    printf("\n");

    for (err = _BIDGetFirstCacheObject(gContext, gContext->AuthorityCache, &k, &j);
         err == BID_S_OK;
         err = _BIDGetNextCacheObject(gContext, gContext->AuthorityCache, &k, &j)) {
        BIDPrintAuthorityCacheEntry(k, j);
        json_decref(j);
        j = NULL;
    }

    if (err == BID_S_NO_MORE_ITEMS)
        err = BID_S_OK;

    json_decref(j);
    return err;
}

static BIDError
BIDPurgeAuthorityCache(int argc, char *argv[])
{
    return BIDPurgeCache(argc, argv, gContext->AuthorityCache, BIDShouldPurgeAuthorityP);
}

static BIDError
BIDDestroyAuthorityCache(int argc, char *argv[])
{
    if (argc)
        BIDToolUsage();

    if (gContext->AuthorityCache == NULL)
        return BID_S_INVALID_PARAMETER;

    return _BIDDestroyCache(gContext, gContext->AuthorityCache);
}

static BIDError
BIDVerifyAssertionFromString(int argc, char *argv[])
{
    BIDError err;
    BIDIdentity identity = NULL;
    time_t expiryTime;
    const char *szExpiryTime;

    if (argc != 2)
        BIDToolUsage();

    err = BIDVerifyAssertion(gContext, argv[0], argv[1], NULL, 0,
                             time(NULL), &identity, &expiryTime);
    if (err != BID_S_OK) {
        BIDAbortError("Failed to verify assertion", err);
        goto cleanup;
    }

    szExpiryTime = expiryTime < gNow ? ctime(&expiryTime) : ">>> Expired <<<";

    printf("Verified assertion for %s issued by %s (expiry %s)\n",
           json_string_value(json_object_get(identity->Attributes, "email")),
           json_string_value(json_object_get(identity->Attributes, "issuer")),
           szExpiryTime);

cleanup:
    BIDReleaseIdentity(gContext, identity);

    return err;
}

static struct {
    const char *Argument;
    const char *Usage;
    BIDError (*Handler)(int argc, char *argv[]);
    enum { NO_CACHE, TICKET_CACHE, REPLAY_CACHE, AUTHORITY_CACHE } CacheUsage;
} _BIDToolHandlers[] = {
    { "tlist",        "", BIDListTicketCache,             TICKET_CACHE         },
    { "tpurge",       "", BIDPurgeTicketCache,            TICKET_CACHE         },
    { "tdestroy",     "", BIDDestroyTicketCache,          TICKET_CACHE         },

    { "rlist",        "", BIDListReplayCache,             REPLAY_CACHE         },
    { "rpurge",       "", BIDPurgeReplayCache,            REPLAY_CACHE         },
    { "rdestroy",     "", BIDDestroyReplayCache,          REPLAY_CACHE         },

    { "certlist",     "", BIDListAuthorityCache,          AUTHORITY_CACHE      },
    { "certpurge",    "", BIDPurgeAuthorityCache,         AUTHORITY_CACHE      },
    { "certdestroy",  "", BIDDestroyAuthorityCache,       AUTHORITY_CACHE      },

    { "verify",       "[assertion] [audience]", BIDVerifyAssertionFromString, NO_CACHE },

};

static void
BIDAbortError(const char *szMessage, BIDError err)
{
    const char *szErrString = NULL;

    if (BIDErrorToString(err, &szErrString) != BID_S_OK)
        szErrString = "Unknown error";

    fprintf(stderr, "bidtool: %s: %s\n", szMessage, szErrString);
    BIDReleaseContext(gContext);
    exit(err);
}

static void
BIDToolUsage(void)
{
    int first = 1;
    int i;

    fprintf(stderr, "Usage: bidtool ");

    for (i = 0; i < sizeof(_BIDToolHandlers) / sizeof(_BIDToolHandlers[0]); i++) {
        if (first) {
            first = 0;
        } else {
            fprintf(stderr, "               ");
        }
        if (_BIDToolHandlers[i].CacheUsage)
            fprintf(stderr, "[-cache name] ");
        fprintf(stderr, "%.20s ", _BIDToolHandlers[i].Argument);
        fprintf(stderr, "%s\n", _BIDToolHandlers[i].Usage);
    }
    exit(BID_S_INVALID_PARAMETER);
}

int main(int argc, char *argv[])
{
    BIDError err;
    uint32_t ulOptions;
    int i;
    uint32_t ulCacheOpt;
    char *szCacheName = NULL;

    ulOptions = BID_CONTEXT_RP              |
                BID_CONTEXT_USER_AGENT      |
                BID_CONTEXT_GSS             |
                BID_CONTEXT_REPLAY_CACHE    |
                BID_CONTEXT_REAUTH          |
                BID_CONTEXT_AUTHORITY_CACHE;

    err = BIDAcquireContext(ulOptions, &gContext);
    if (err != BID_S_OK)
        BIDAbortError("Failed to acquire context", err);

    if (argc < 2)
        BIDToolUsage();

    argc--;
    argv++;

    gNow = time(NULL);

    if (argc > 1 && strcmp(argv[0], "-cache") == 0) {
        if (argc < 2)
            BIDToolUsage();
        szCacheName = argv[1];
        argc -= 2;
        argv += 2;
    }

    err = BID_S_INVALID_PARAMETER;

    for (i = 0; i < sizeof(_BIDToolHandlers) / sizeof(_BIDToolHandlers[0]); i++) {
        if (strcmp(argv[0], _BIDToolHandlers[i].Argument) == 0) {
            switch (_BIDToolHandlers[i].CacheUsage) {
            case TICKET_CACHE:
                ulCacheOpt = BID_PARAM_TICKET_CACHE;
                break;
            case REPLAY_CACHE:
                ulCacheOpt = BID_PARAM_REPLAY_CACHE;
                break;
            case AUTHORITY_CACHE:
                ulCacheOpt = BID_PARAM_AUTHORITY_CACHE;
                break;
            default:
                ulCacheOpt = 0;
                break;
            }
            if (szCacheName != NULL && ulCacheOpt != 0) {
                err = BIDSetContextParam(gContext, ulCacheOpt, szCacheName);
                if (err != BID_S_OK)
                    BIDAbortError("Failed to acquire cache", err);
            }

            argc--;
            argv++;

            err = _BIDToolHandlers[i].Handler(argc, argv);
            break;
        }
    }

    if (err == BID_S_INVALID_PARAMETER)
        BIDToolUsage();

    BIDReleaseContext(gContext);

    exit(0);
}
