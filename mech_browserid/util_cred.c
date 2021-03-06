/*
 * Copyright (c) 2011, JANET(UK)
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
 * 3. Neither the name of JANET(UK) nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Utility routines for credential handles.
 */

#include "gssapiP_bid.h"

OM_uint32
gssBidAllocCred(OM_uint32 *minor, gss_cred_id_t *pCred)
{
    OM_uint32 major, tmpMinor;
    gss_cred_id_t cred;
    BIDError err;

    *pCred = GSS_C_NO_CREDENTIAL;

    cred = (gss_cred_id_t)GSSBID_CALLOC(1, sizeof(*cred));
    if (cred == GSS_C_NO_CREDENTIAL) {
        *minor = ENOMEM;
        return GSS_S_FAILURE;
    }

    if (GSSBID_MUTEX_INIT(&cred->mutex) != 0) {
        *minor = GSSBID_GET_LAST_ERROR();
        gssBidReleaseCred(&tmpMinor, &cred);
        return GSS_S_FAILURE;
    }

    err = BIDAcquireContext(GSSBID_CONFIG_FILE, BID_CONTEXT_GSS, NULL, &cred->bidContext);
    if (err != BID_S_OK) {
        major = gssBidMapError(minor, err);
        gssBidReleaseCred(&tmpMinor, &cred);
        return GSS_ERROR(major) ? major : GSS_S_FAILURE;
    }

    *pCred = cred;

    *minor = 0;
    return GSS_S_COMPLETE;
}

OM_uint32
gssBidReleaseCred(OM_uint32 *minor, gss_cred_id_t *pCred)
{
    OM_uint32 tmpMinor;
    gss_cred_id_t cred = *pCred;
    krb5_context krbContext = NULL;

    if (cred == GSS_C_NO_CREDENTIAL) {
        return GSS_S_COMPLETE;
    }

    GSSBID_KRB_INIT(&krbContext);

    gssBidReleaseName(&tmpMinor, &cred->name);
    gssBidReleaseName(&tmpMinor, &cred->target);
    gss_release_buffer(&tmpMinor, &cred->assertion);
    gss_release_oid_set(&tmpMinor, &cred->mechanisms);
    if (cred->bidContext != BID_C_NO_CONTEXT) {
        BIDReleaseTicketCache(cred->bidContext, cred->bidTicketCache);
        BIDReleaseReplayCache(cred->bidContext, cred->bidReplayCache);
        BIDReleaseContext(cred->bidContext);
    }
#ifdef __APPLE__
    if (cred->bidIdentity)
        CFRelease(cred->bidIdentity);
#endif

    GSSBID_MUTEX_DESTROY(&cred->mutex);
    memset(cred, 0, sizeof(*cred));
    GSSBID_FREE(cred);
    *pCred = NULL;

    *minor = 0;
    return GSS_S_COMPLETE;
}

gss_OID
gssBidPrimaryMechForCred(gss_cred_id_t cred)
{
    gss_OID credMech = GSS_C_NO_OID;

    if (cred != GSS_C_NO_CREDENTIAL &&
        cred->mechanisms != GSS_C_NO_OID_SET &&
        cred->mechanisms->count == 1)
        credMech = &cred->mechanisms->elements[0];

    return credMech;
}

static OM_uint32
gssBidSetCredMechs(OM_uint32 *minor,
                   gss_cred_id_t cred,
                   gss_OID_set mechs)
{
    OM_uint32 major, tmpMinor;
    gss_OID_set newMechs = GSS_C_NO_OID_SET;

    major = duplicateOidSet(minor, mechs, &newMechs);
    if (GSS_ERROR(major))
        return major;

    gss_release_oid_set(&tmpMinor, &cred->mechanisms);
    cred->mechanisms = newMechs;

    return GSS_S_COMPLETE;
}

static OM_uint32
gssBidSetCredName(OM_uint32 *minor,
                  gss_cred_id_t cred,
                  gss_name_t name,
                  int freeIt)
{
    OM_uint32 major, tmpMinor;
    gss_name_t newName;

    if (freeIt == 0) {
        GSSBID_MUTEX_LOCK(&name->mutex);
        major = gssBidDuplicateName(minor, name, &newName);
        GSSBID_MUTEX_UNLOCK(&name->mutex);

        if (GSS_ERROR(major))
            return major;
    } else {
        newName = name;
    }

    gssBidReleaseName(&tmpMinor, &cred->name);
    cred->name = newName;

    return GSS_S_COMPLETE;
}

OM_uint32
gssBidAcquireCred(OM_uint32 *minor,
                  const gss_name_t desiredName,
                  OM_uint32 timeReq GSSBID_UNUSED,
                  const gss_OID_set desiredMechs,
                  int credUsage,
                  gss_cred_id_t *pCred,
                  gss_OID_set *pActualMechs,
                  OM_uint32 *timeRec)
{
    OM_uint32 major, tmpMinor;
    gss_cred_id_t cred;

    /* XXX TODO validate with changed set_cred_option API */
    *pCred = GSS_C_NO_CREDENTIAL;

    major = gssBidAllocCred(minor, &cred);
    if (GSS_ERROR(major))
        goto cleanup;

    switch (credUsage) {
    case GSS_C_BOTH:
        cred->flags |= CRED_FLAG_INITIATE | CRED_FLAG_ACCEPT;
        break;
    case GSS_C_INITIATE:
        cred->flags |= CRED_FLAG_INITIATE;
        break;
    case GSS_C_ACCEPT:
        cred->flags |= CRED_FLAG_ACCEPT;
        break;
    default:
        major = GSS_S_FAILURE;
        *minor = GSSBID_BAD_USAGE;
        goto cleanup;
        break;
    }

#ifdef GSS_C_CRED_NO_UI
    if (credUsage & GSS_C_CRED_NO_UI)
        cred->flags |= CRED_FLAG_CALLER_UI;
#endif

    major = gssBidValidateMechs(minor, desiredMechs);
    if (GSS_ERROR(major))
        goto cleanup;

    major = gssBidSetCredMechs(minor, cred, desiredMechs);
    if (GSS_ERROR(major))
        goto cleanup;

    if (desiredName != GSS_C_NO_NAME) {
        major = gssBidSetCredName(minor, cred, desiredName, 0);
        if (GSS_ERROR(major)) {
            GSSBID_MUTEX_UNLOCK(&desiredName->mutex);
            goto cleanup;
        }
    }

    if (pActualMechs != NULL) {
        major = duplicateOidSet(minor, cred->mechanisms, pActualMechs);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    if (timeRec != NULL)
        *timeRec = GSS_C_INDEFINITE;

    *pCred = cred;

    major = GSS_S_COMPLETE;
    *minor = 0;

cleanup:
    if (GSS_ERROR(major))
        gssBidReleaseCred(&tmpMinor, &cred);

    return major;
}

/*
 * Return TRUE if cred available for mechanism. Caller need no acquire
 * lock because mechanisms list is immutable.
 */
int
gssBidCredAvailable(gss_cred_id_t cred, gss_OID mech)
{
    OM_uint32 minor;
    int present = 0;

    GSSBID_ASSERT(mech != GSS_C_NO_OID);

    if (cred == GSS_C_NO_CREDENTIAL || cred->mechanisms == GSS_C_NO_OID_SET)
        return TRUE;

    gss_test_oid_set_member(&minor, mech, cred->mechanisms, &present);

    return present;
}

OM_uint32
gssBidInquireCred(OM_uint32 *minor,
                  gss_cred_id_t cred,
                  gss_name_t *name,
                  OM_uint32 *pLifetime,
                  gss_cred_usage_t *cred_usage,
                  gss_OID_set *mechanisms)
{
    OM_uint32 major;
    time_t now, lifetime;

    if (name != NULL) {
        if (cred->name != GSS_C_NO_NAME) {
            major = gssBidDuplicateName(minor, cred->name, name);
            if (GSS_ERROR(major))
                goto cleanup;
        } else
            *name = GSS_C_NO_NAME;
    }

    if (cred_usage != NULL) {
        OM_uint32 flags = (cred->flags & (CRED_FLAG_INITIATE | CRED_FLAG_ACCEPT));

        switch (flags) {
        case CRED_FLAG_INITIATE:
            *cred_usage = GSS_C_INITIATE;
            break;
        case CRED_FLAG_ACCEPT:
            *cred_usage = GSS_C_ACCEPT;
            break;
        default:
            *cred_usage = GSS_C_BOTH;
            break;
        }
    }

    if (mechanisms != NULL) {
        if (cred->mechanisms != GSS_C_NO_OID_SET)
            major = duplicateOidSet(minor, cred->mechanisms, mechanisms);
        else
            major = gssBidIndicateMechs(minor, mechanisms);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    if (cred->expiryTime == 0) {
        lifetime = GSS_C_INDEFINITE;
    } else  {
        now = time(NULL);
        lifetime = now - cred->expiryTime;
        if (lifetime < 0)
            lifetime = 0;
    }

    if (pLifetime != NULL) {
        *pLifetime = lifetime;
    }

    if (lifetime == 0) {
        major = GSS_S_CREDENTIALS_EXPIRED;
        *minor = GSSBID_CRED_EXPIRED;
        goto cleanup;
    }

    major = GSS_S_COMPLETE;
    *minor = 0;

cleanup:
    return major;
}

OM_uint32
gssBidSetCredAssertion(OM_uint32 *minor,
                       gss_cred_id_t cred,
                       const gss_buffer_t assertion)
{
    OM_uint32 major, tmpMinor;
    gss_buffer_desc newAssertion = GSS_C_EMPTY_BUFFER;

    if (cred->flags & CRED_FLAG_RESOLVED) {
        major = GSS_S_FAILURE;
        *minor = GSSBID_CRED_RESOLVED;
        goto cleanup;
    }

    if (assertion != GSS_C_NO_BUFFER) {
        major = duplicateBuffer(minor, assertion, &newAssertion);
        if (GSS_ERROR(major))
            goto cleanup;

        cred->flags |= CRED_FLAG_ASSERTION | CRED_FLAG_RESOLVED;
    } else {
        cred->flags &= ~(CRED_FLAG_ASSERTION);
    }

    gss_release_buffer(&tmpMinor, &cred->assertion);
    cred->assertion = newAssertion;

    major = GSS_S_COMPLETE;
    *minor = 0;

cleanup:
    return major;
}

OM_uint32
gssBidSetCredService(OM_uint32 *minor,
                     gss_cred_id_t cred,
                     const gss_name_t target)
{
    OM_uint32 major, tmpMinor;
    gss_name_t newTarget = GSS_C_NO_NAME;

    if (cred->flags & CRED_FLAG_RESOLVED) {
        major = GSS_S_FAILURE;
        *minor = GSSBID_CRED_RESOLVED;
        goto cleanup;
    }

    if (target != GSS_C_NO_NAME) {
        major = gssBidDuplicateName(minor, target, &newTarget);
        if (GSS_ERROR(major))
            goto cleanup;

        cred->flags |= CRED_FLAG_TARGET;
    } else {
        cred->flags &= ~(CRED_FLAG_TARGET);
    }

    gssBidReleaseName(&tmpMinor, &cred->target);
    cred->target = newTarget;

    major = GSS_S_COMPLETE;
    *minor = 0;

cleanup:
    return major;
}

OM_uint32
gssBidSetCredTicketCacheName(OM_uint32 *minor,
                             gss_cred_id_t cred,
                             const gss_buffer_t cacheName)
{
    OM_uint32 major;
    BIDError err;
    BIDCache newCache = BID_C_NO_TICKET_CACHE;

    if (cred->flags & CRED_FLAG_RESOLVED) {
        major = GSS_S_FAILURE;
        *minor = GSSBID_CRED_RESOLVED;
        goto cleanup;
    }

    if (cacheName != GSS_C_NO_BUFFER) {
        err = BIDAcquireTicketCache(cred->bidContext, (char *)cacheName->value, &newCache);
        if (err != BID_S_OK) {
            major = gssBidMapError(minor, err);
            goto cleanup;
        }
    }

    BIDReleaseTicketCache(cred->bidContext, cred->bidTicketCache);
    cred->bidTicketCache = newCache;

    major = GSS_S_COMPLETE;
    *minor = 0;

cleanup:
    return major;
}

OM_uint32
gssBidSetCredReplayCacheName(OM_uint32 *minor,
                             gss_cred_id_t cred,
                             const gss_buffer_t cacheName)
{
    OM_uint32 major;
    BIDError err;
    BIDCache newCache = BID_C_NO_REPLAY_CACHE;

    if (cred->flags & CRED_FLAG_RESOLVED) {
        major = GSS_S_FAILURE;
        *minor = GSSBID_CRED_RESOLVED;
        goto cleanup;
    }

    if (cacheName != GSS_C_NO_BUFFER) {
        err = BIDAcquireReplayCache(cred->bidContext, (char *)cacheName->value, &newCache);
        if (err != BID_S_OK) {
            major = gssBidMapError(minor, err);
            goto cleanup;
        }
    }

    BIDReleaseReplayCache(cred->bidContext, cred->bidReplayCache);
    cred->bidReplayCache = newCache;

    major = GSS_S_COMPLETE;
    *minor = 0;

cleanup:
    return major;
}

static OM_uint32
gssBidDuplicateCred(OM_uint32 *minor,
                    const gss_cred_id_t src,
                    gss_cred_id_t *pDst)
{
    OM_uint32 major, tmpMinor;
    gss_cred_id_t dst = GSS_C_NO_CREDENTIAL;

    *pDst = GSS_C_NO_CREDENTIAL;

    GSSBID_ASSERT(src != GSS_C_NO_CREDENTIAL);

    major = gssBidAllocCred(minor, &dst);
    if (GSS_ERROR(major))
        goto cleanup;

    GSSBID_ASSERT(dst != GSS_C_NO_CREDENTIAL);

    dst->flags = src->flags;

    if (src->name != GSS_C_NO_NAME) {
        major = gssBidSetCredName(minor, dst, src->name, 0);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    if (src->target != GSS_C_NO_NAME) {
        major = gssBidSetCredService(minor, dst, src->target);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    if (src->assertion.value != NULL) {
        major = duplicateBuffer(minor, &src->assertion, &dst->assertion);
        if (GSS_ERROR(major))
            goto cleanup;
    }

#ifdef __APPLE__
    if (src->bidIdentity)
        dst->bidIdentity = (BIDIdentity)CFRetain(src->bidIdentity);
    dst->bidFlags = src->bidFlags;
#endif

    major = gssBidSetCredMechs(minor, dst, src->mechanisms);
    if (GSS_ERROR(major))
        goto cleanup;

    dst->expiryTime = src->expiryTime;

    *pDst = dst;
    dst = GSS_C_NO_CREDENTIAL;

    major = GSS_S_COMPLETE;
    *minor = 0;

cleanup:
    gssBidReleaseCred(&tmpMinor, &dst);

    return major;
}

OM_uint32
gssBidResolveInitiatorCred(OM_uint32 *minor,
                           const gss_cred_id_t cred,
                           gss_ctx_id_t ctx,
                           const gss_name_t targetName,
                           OM_uint32 req_flags,
                           const gss_channel_bindings_t channelBindings)
{
    OM_uint32 major, tmpMinor;
    gss_cred_id_t resolvedCred = GSS_C_NO_CREDENTIAL;
    BIDError err;
    gss_buffer_desc bufAudienceOrSpn = GSS_C_EMPTY_BUFFER;
    gss_buffer_desc bufSubject = GSS_C_EMPTY_BUFFER;
    gss_name_t identityName = GSS_C_NO_NAME;
    const unsigned char *pbChannelBindings = NULL;
    size_t cbChannelBindings = 0;
    char *szAssertion = NULL;
    uint32_t ulRetFlags = 0;

    if (ctx->cred != GSS_C_NO_CREDENTIAL) {
        GSSBID_ASSERT(resolvedCred->flags & CRED_FLAG_RESOLVED);
        GSSBID_ASSERT(resolvedCred->assertion.length != 0);

        major = GSS_S_COMPLETE;
        goto cleanup;
    }

    if (cred == GSS_C_NO_CREDENTIAL) {
        major = gssBidAcquireCred(minor,
                                  GSS_C_NO_NAME,
                                  GSS_C_INDEFINITE,
                                  GSS_C_NO_OID_SET,
                                  GSS_C_INITIATE,
                                  &resolvedCred,
                                  NULL,
                                  NULL);
        if (GSS_ERROR(major))
            goto cleanup;
    } else {
        if ((cred->flags & CRED_FLAG_INITIATE) == 0) {
            major = GSS_S_NO_CRED;
            *minor = GSSBID_CRED_USAGE_MISMATCH;
            goto cleanup;
        }

        major = gssBidDuplicateCred(minor, cred, &resolvedCred);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    /*
     * If building for CredUI, and the initiator tried to re-authenticate and
     * it failed, don't acquire a credential here because we can't show UI.
     * Just return an error to the application.
     */
    if ((resolvedCred->flags & CRED_FLAG_CALLER_UI) && (ctx->flags & CTX_FLAG_REAUTH)) {
        ctx->flags &= ~(CTX_FLAG_REAUTH);
        GSSBID_SM_TRANSITION(ctx, GSSBID_STATE_RETRY_INITIAL);
        major = GSS_S_FAILURE | GSS_S_PROMPTING_NEEDED;
        *minor = GSSBID_REAUTH_FAILED;
        goto cleanup;
    }

    if (resolvedCred->flags & CRED_FLAG_RESOLVED) {
#ifdef __APPLE__
        if (resolvedCred->bidIdentity != BID_C_NO_IDENTITY) {
            ctx->bidIdentity = (BIDIdentity)CFRetain(cred->bidIdentity);
            ctx->flags &= ~(CTX_FLAG_REAUTH);
            err = BID_S_OK;
        } else
#endif
        err = BIDAcquireAssertionFromString(ctx->bidContext,
                                            (const char *)resolvedCred->assertion.value,
                                            BID_ACQUIRE_FLAG_NO_INTERACT,
                                            &ctx->bidIdentity,
                                            &resolvedCred->expiryTime,
                                            &ulRetFlags);
    } else {
        uint32_t ulReqFlags;

        if (channelBindings != GSS_C_NO_CHANNEL_BINDINGS) {
            pbChannelBindings = (const unsigned char *)channelBindings->application_data.value;
            cbChannelBindings = channelBindings->application_data.length;
        }

        if (targetName != GSS_C_NO_NAME) {
            major = gssBidDisplayName(minor, targetName, &bufAudienceOrSpn, NULL);
            if (GSS_ERROR(major))
                goto cleanup;
        }

        if (resolvedCred->name != GSS_C_NO_NAME) {
            major = gssBidDisplayName(minor, resolvedCred->name, &bufSubject, NULL);
            if (GSS_ERROR(major))
                goto cleanup;
        }

        ulReqFlags = 0;
        if (resolvedCred->flags & CRED_FLAG_CALLER_UI)
            ulReqFlags |= BID_ACQUIRE_FLAG_NO_INTERACT;
        if (ctx->flags & CTX_FLAG_REAUTH)
            ulReqFlags |= BID_ACQUIRE_FLAG_NO_CACHED;
        if (req_flags & GSS_C_MUTUAL_FLAG)
            ulReqFlags |= BID_ACQUIRE_FLAG_MUTUAL_AUTH;
        if (req_flags & GSS_C_DCE_STYLE)
            ulReqFlags |= BID_ACQUIRE_FLAG_EXTRA_ROUND_TRIP | BID_ACQUIRE_FLAG_DCE;
        if (req_flags & GSS_C_IDENTIFY_FLAG)
            ulReqFlags |= BID_ACQUIRE_FLAG_IDENTIFY;

        err = BIDAcquireAssertion(ctx->bidContext,
                                  (cred == GSS_C_NO_CREDENTIAL) ? BID_C_NO_TICKET_CACHE : cred->bidTicketCache,
                                  (targetName == GSS_C_NO_NAME) ? NULL : (const char *)bufAudienceOrSpn.value,
                                  pbChannelBindings,
                                  cbChannelBindings,
                                  (const char *)bufSubject.value,
                                  ulReqFlags,
                                  &szAssertion,
                                  &ctx->bidIdentity,
                                  &resolvedCred->expiryTime,
                                  &ulRetFlags);

        gss_release_buffer(&tmpMinor, &bufSubject);
    }
    if (err != BID_S_OK) {
        major = gssBidMapError(minor, err);
        goto cleanup;
    }

    if (ulRetFlags & BID_ACQUIRE_FLAG_REAUTH)
        ctx->flags |= CTX_FLAG_REAUTH;
    else
        ctx->flags &= ~(CTX_FLAG_REAUTH);
    if (ulRetFlags & BID_ACQUIRE_FLAG_REAUTH_MUTUAL)
        ctx->gssFlags |= GSS_C_MUTUAL_FLAG;
    else
        ctx->gssFlags &= ~(GSS_C_MUTUAL_FLAG);

    if (szAssertion != NULL) {
        major = makeStringBuffer(minor, szAssertion, &resolvedCred->assertion);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    BID_ASSERT(resolvedCred->assertion.length != 0);

    err = BIDGetIdentitySubject(ctx->bidContext, ctx->bidIdentity, (const char **)&bufSubject.value);
    if (err != BID_S_OK) {
        major = gssBidMapError(minor, err);
        goto cleanup;
    }

    bufSubject.length = strlen((const char *)bufSubject.value);

    major = gssBidImportName(minor, &bufSubject, GSS_C_NT_USER_NAME, GSS_C_NULL_OID, &identityName);
    if (GSS_ERROR(major))
        goto cleanup;

    if (resolvedCred->name != GSS_C_NO_NAME) {
        int nameEqual;

        major = gssBidCompareName(minor, resolvedCred->name, identityName, 0, &nameEqual);
        if (GSS_ERROR(major))
            goto cleanup;

        if (!nameEqual) {
            major = GSS_S_NO_CRED;
            *minor = GSSBID_BAD_INITIATOR_NAME;
            goto cleanup;
        }
    } else {
        resolvedCred->name = identityName;
        identityName = GSS_C_NO_NAME;
    }

    resolvedCred->flags |= CRED_FLAG_RESOLVED;

    ctx->cred = resolvedCred;
    resolvedCred = GSS_C_NO_CREDENTIAL;

    major = GSS_S_COMPLETE;
    *minor = 0;

cleanup:
    gssBidReleaseCred(&tmpMinor, &resolvedCred);
    gssBidReleaseName(&tmpMinor, &identityName);
    gss_release_buffer(&tmpMinor, &bufAudienceOrSpn);
    BIDFreeAssertion(ctx->bidContext, szAssertion);

    return major;
}

OM_uint32
gssBidImportCred(OM_uint32 *minor,
                 gss_buffer_t credToken,
                 gss_cred_id_t *pCredHandle)
{
    OM_uint32 major, tmpMinor;
    gss_cred_id_t cred = GSS_C_NO_CREDENTIAL;
    json_t *credObject = NULL, *tmp;
    json_error_t jsonError;
    char *szJson = NULL;
#ifndef HAVE_HEIMDAL_VERSION
    /* XXX Heimdal is asymmetrical: we encode the OID but the mechglue decodes it */
    unsigned char *p = (unsigned char *)credToken->value;
    size_t remain = credToken->length;
    gss_OID credMech = GSS_C_NO_OID;
    gss_buffer_desc tmpBuffer;

    major = gssBidImportMechanismOid(minor, &p, &remain, &credMech);
    if (GSS_ERROR(major))
        goto cleanup;

    if (!gssBidIsMechanismOid(credMech)) {
        major = GSS_S_DEFECTIVE_TOKEN;
        goto cleanup;
    }

    tmpBuffer.length = remain;
    tmpBuffer.value = p;

    major = bufferToString(minor, &tmpBuffer, &szJson);
#else
    major = bufferToString(minor, credToken, &szJson);
#endif /* !HAVE_HEIMDAL_VERSION */
    if (GSS_ERROR(major))
        goto cleanup;

    credObject = json_loads(szJson, 0, &jsonError);
    if (credObject == NULL) {
        major = GSS_S_DEFECTIVE_TOKEN;
        goto cleanup;
    }

    major = gssBidAllocCred(minor, &cred);
    if (GSS_ERROR(major))
        goto cleanup;

    cred->flags = json_integer_value(json_object_get(credObject, "flags"));
    cred->name = gssBidImportNameJson(json_object_get(credObject, "name"));
    cred->target = gssBidImportNameJson(json_object_get(credObject, "target"));

    tmp = json_object_get(credObject, "assertion");
    if (tmp != NULL && json_is_string(tmp)) {
        major = makeStringBuffer(minor, json_string_value(tmp), &cred->assertion);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    tmp = json_object_get(credObject, "mechanisms");
    if (tmp != NULL && json_is_array(tmp)) {
        gss_OID_set mechanisms = GSS_C_NO_OID_SET;

        major = jsonToOidSet(minor, tmp, &mechanisms);
        if (GSS_ERROR(major))
            goto cleanup;

        major = gssBidSetCredMechs(minor, cred, mechanisms);
        if (GSS_ERROR(major)) {
            gss_release_oid_set(&tmpMinor, &mechanisms);
            goto cleanup;
        }

        gss_release_oid_set(&tmpMinor, &mechanisms);
    }

    cred->expiryTime = json_integer_value(json_object_get(credObject, "expiry-time"));

    tmp = json_object_get(credObject, "ticket-cache");
    if (tmp != NULL && json_is_string(tmp)) {
        gss_buffer_desc buf = { strlen(json_string_value(tmp)), (void *)json_string_value(tmp) };

        major = gssBidSetCredTicketCacheName(minor, cred, &buf);
        if (GSS_ERROR(major))
            goto cleanup;
    } 

    tmp = json_object_get(credObject, "replay-cache");
    if (tmp != NULL && json_is_string(tmp)) {
        gss_buffer_desc buf = { strlen(json_string_value(tmp)), (void *)json_string_value(tmp) };

        major = gssBidSetCredReplayCacheName(minor, cred, &buf);
        if (GSS_ERROR(major))
            goto cleanup;
    } 

#ifdef __APPLE__
    tmp = json_object_get(credObject, "bid-identity");
    if (tmp != NULL && json_is_object(tmp)) {
        BIDError err;

        err = _BIDAllocIdentity(cred->bidContext, json_object_get(tmp, "attributes"), &cred->bidIdentity);
        if (err != BID_S_OK) {
            major = gssBidMapError(minor, err);
            goto cleanup;
        }

        tmp = json_object_get(tmp, "privateAttributes");
        if (tmp != NULL && json_is_object(tmp))
            cred->bidIdentity->PrivateAttributes = json_incref(tmp);
    }

    cred->bidFlags = json_integer_value(json_object_get(credObject, "bid-flags"));
#endif /* __APPLE__ */

    *pCredHandle = cred;
    cred = GSS_C_NO_CREDENTIAL;

cleanup:
    json_decref(credObject);
    GSSBID_FREE(szJson);

    if (GSS_ERROR(major))
        gssBidReleaseCred(&tmpMinor, &cred);

    return major;
}

OM_uint32
gssBidExportCred(OM_uint32 *minor,
                 gss_cred_id_t cred,
                 gss_buffer_t credToken)
{
    OM_uint32 major;
    gss_OID credMech;
    json_t *credObject;
    const char *cacheName;
    unsigned char *p;
    gss_buffer_desc jsonBuffer;

    if (credToken == GSS_C_NO_BUFFER)
        return GSS_S_CALL_INACCESSIBLE_READ | GSS_S_FAILURE;

    credObject = json_object();
    if (credObject == NULL) {
        *minor = ENOMEM;
        return GSS_S_FAILURE;
    }

    json_object_set_new(credObject, "flags", json_integer(cred->flags));
    if (cred->name != GSS_C_NO_NAME)
        json_object_set_new(credObject, "name", gssBidExportNameJson(cred->name));
    if (cred->target != GSS_C_NO_NAME)
        json_object_set_new(credObject, "target", gssBidExportNameJson(cred->target));
    if (cred->assertion.value != NULL)
        json_object_set_new(credObject, "assertion", json_string((char *)cred->assertion.value));
    if (cred->mechanisms != GSS_C_NO_OID_SET) {
        json_t *mechs;

        major = oidSetToJson(minor, cred->mechanisms, &mechs);
        if (GSS_ERROR(major)) {
            json_decref(credObject);
            return major;
        }
        json_object_set_new(credObject, "mechanisms", mechs);
    }

    json_object_set_new(credObject, "expiry-time", json_integer(cred->expiryTime));

    _BIDGetCacheName(cred->bidContext, cred->bidTicketCache, &cacheName);
    if (cacheName != NULL)
        json_object_set_new(credObject, "ticket-cache", json_string(cacheName));
    _BIDGetCacheName(cred->bidContext, cred->bidReplayCache, &cacheName);
    if (cacheName != NULL)
        json_object_set_new(credObject, "replay-cache", json_string(cacheName));

#ifdef __APPLE__
    if (cred->bidIdentity != BID_C_NO_IDENTITY) {
        json_t *bidIdentity = json_object();

        if (cred->bidIdentity->Attributes != NULL)
            json_object_set(bidIdentity, "attributes", cred->bidIdentity->Attributes);
        if (cred->bidIdentity->PrivateAttributes != NULL)
            json_object_set(bidIdentity, "privateAttributes", cred->bidIdentity->PrivateAttributes);

        json_object_set_new(credObject, "bid-identity", bidIdentity);
    }

    json_object_set_new(credObject, "bid-flags", json_integer(cred->bidFlags));
#endif /* __APPLE__ */

    major = gssBidCanonicalizeOid(minor,
                                  cred->mechanisms ? &cred->mechanisms->elements[0] : GSS_C_NO_OID,
                                  OID_FLAG_NULL_VALID | OID_FLAG_MAP_NULL_TO_DEFAULT_MECH,
                                  &credMech);
    if (GSS_ERROR(major)) {
        json_decref(credObject);
        return major;
    }

    jsonBuffer.value = json_dumps(credObject, JSON_COMPACT);
    if (jsonBuffer.value == NULL) {
        *minor = ENOMEM;
        json_decref(credObject);
        return GSS_S_FAILURE;
    }

    jsonBuffer.length = strlen((char *)jsonBuffer.value);

    credToken->length = 4 + credMech->length + 4 + jsonBuffer.length;
    credToken->value = BIDMalloc(credToken->length);
    if (credToken->value == NULL) {
        *minor = ENOMEM;
        json_decref(credObject);
        BIDFree(jsonBuffer.value);
        return GSS_S_FAILURE;
    }

    p = store_oid(credMech, (unsigned char *)credToken->value);
    p = store_buffer(&jsonBuffer, p, FALSE);

    assert(p == (unsigned char *)credToken->value + credToken->length);

    json_decref(credObject);
    BIDFree(jsonBuffer.value);

    *minor = 0;
    return GSS_S_COMPLETE;
}

#if defined(__APPLE__) && defined(HAVE_HEIMDAL_VERSION)

#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <GSS/gssapi_apple.h>

#define kGSSICBrowserIDAssertion        CFSTR("kGSSICBrowserIDAssertion")
#define kGSSICBrowserIDIdentity         CFSTR("kGSSICBrowserIDIdentity")
#define kGSSICBrowserIDFlags            CFSTR("kGSSICBrowserIDFlags")

#define kGSSCredentialName              CFSTR("kGSSCredentialName")
#define kGSSCredentialMechanismOID      CFSTR("kGSSCredentialMechanismOID")

#define kGSSCredentialUsage             CFSTR("kGSSCredentialUsage")
#define kGSS_C_INITIATE                 CFSTR("kGSS_C_INITIATE")
#define kGSS_C_ACCEPT                   CFSTR("kGSS_C_ACCEPT")
#define kGSS_C_BOTH                     CFSTR("kGSS_C_BOTH")

static OM_uint32
cfStringToGssBuffer(OM_uint32 *minor,
                    CFStringRef cfString,
                    gss_buffer_t buffer)
{
    if (cfString == NULL || CFGetTypeID(cfString) != CFStringGetTypeID())
        return GSS_S_CALL_INACCESSIBLE_READ | GSS_S_FAILURE;

    if (CFStringGetLength(cfString) == 0) {
        *minor = ENOENT;
        return GSS_S_FAILURE;
    }

    buffer->length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(cfString),
                                                       kCFStringEncodingUTF8);
    buffer->value = GSSBID_MALLOC(buffer->length + 1);
    if (buffer->value == NULL) {
        *minor = ENOMEM;
        return GSS_S_FAILURE;
    }

    if (!CFStringGetCString(cfString, buffer->value,
                            buffer->length, kCFStringEncodingUTF8)) {
        OM_uint32 tmpMinor;
        *minor = EINVAL;
        gss_release_buffer(&tmpMinor, buffer);
        return GSS_S_FAILURE;
    }

    *minor = 0;
    return GSS_S_COMPLETE;
}

OM_uint32
gssBidSetCredWithCFDictionary(OM_uint32 *minor,
                              gss_cred_id_t cred,
                              CFDictionaryRef attrs)
{
    OM_uint32 major = GSS_S_COMPLETE, tmpMinor;
    CFStringRef credUsage;
    CFStringRef assertion;
    gss_buffer_desc assertionBuf = GSS_C_EMPTY_BUFFER;
    gss_name_t desiredName;
    gss_buffer_desc nameBuf = GSS_C_EMPTY_BUFFER;
    gss_OID oid = GSS_C_NO_OID;
    CFStringRef desiredMechOid;
    BIDIdentity identity;
    CFNumberRef bidFlags;

    desiredMechOid = (CFStringRef)CFDictionaryGetValue(attrs, kGSSCredentialMechanismOID);
    if (desiredMechOid != NULL) {
        gss_OID canonOid;
        gss_OID_set_desc desiredMechs;

        major = jsonToOid(minor, desiredMechOid, &oid);
        if (GSS_ERROR(major))
            goto cleanup;

        major = gssBidCanonicalizeOid(minor, oid, 0, &canonOid);
        if (GSS_ERROR(major)) {
            if (major == GSS_S_BAD_MECH)
                major = GSS_S_CRED_UNAVAIL;
            goto cleanup;
        }

        desiredMechs.count = 1;
        desiredMechs.elements = canonOid;

        major = gssBidSetCredMechs(minor, cred, &desiredMechs);
        if (GSS_ERROR(major))
            goto cleanup;
    }

    credUsage = (CFStringRef)CFDictionaryGetValue(attrs, kGSSCredentialUsage);
    if (credUsage != NULL) {
        if (CFEqual(credUsage, kGSS_C_INITIATE))
            cred->flags |= CRED_FLAG_INITIATE;
        else if (CFEqual(credUsage, kGSS_C_ACCEPT))
            cred->flags |= CRED_FLAG_ACCEPT;
        else if (CFEqual(credUsage, kGSS_C_BOTH))
            cred->flags |= CRED_FLAG_INITIATE | CRED_FLAG_ACCEPT;
    }

    desiredName = (gss_name_t)CFDictionaryGetValue(attrs, kGSSCredentialName);
    if (desiredName != NULL) {
        gss_OID nameType = GSS_C_NO_OID;
        gss_name_t gssBidName = GSS_C_NO_NAME;
        OM_uint32 (*gssDisplayNameFn)(OM_uint32 *, const gss_name_t, gss_buffer_t, gss_OID *) = dlsym(RTLD_NEXT, "gss_display_name");

        GSSBID_ASSERT(gssDisplayNameFn != NULL);

        /* convert from mechglue name to string, then to MN */
        major = gssDisplayNameFn(minor, desiredName, &nameBuf, &nameType);
        if (GSS_ERROR(major))
            goto cleanup;

        major = gssBidImportName(minor, &nameBuf, nameType, GSS_C_NULL_OID, &gssBidName);
        if (GSS_ERROR(major))
            goto cleanup;

        gssBidSetCredName(minor, cred, gssBidName, 1);
    }

    assertion = (CFStringRef)CFDictionaryGetValue(attrs, kGSSICBrowserIDAssertion);
    if (assertion != NULL) {
        major = cfStringToGssBuffer(minor, assertion, &assertionBuf);
        if (GSS_ERROR(major))
            goto cleanup;

        major = gssBidSetCredAssertion(minor, cred, &assertionBuf);
        if (GSS_ERROR(major))
            goto cleanup;

        GSSBID_ASSERT(cred->flags & CRED_FLAG_RESOLVED);
    }

    identity = (BIDIdentity)CFDictionaryGetValue(attrs, kGSSICBrowserIDIdentity);
    if (identity != BID_C_NO_IDENTITY && CFGetTypeID(identity) == BIDIdentityGetTypeID())
        cred->bidIdentity = (BIDIdentity)CFRetain(identity);

    bidFlags = (CFNumberRef)CFDictionaryGetValue(attrs, kGSSICBrowserIDFlags);
    if (bidFlags != NULL && CFGetTypeID(bidFlags) == CFNumberGetTypeID())
        CFNumberGetValue(bidFlags, kCFNumberSInt32Type, (void *)&cred->bidFlags);

    /* in case the dictionary wasn't filled out correctly, assume we're an initiator */
    if ((cred->flags & (CRED_FLAG_INITIATE | CRED_FLAG_ACCEPT)) == 0)
        cred->flags |= CRED_FLAG_INITIATE;

    /* Caller must display UI, we just return GSS_S_PROMPTING_NEEDED */
    if (cred->flags & CRED_FLAG_INITIATE)
        cred->flags |= CRED_FLAG_CALLER_UI;

cleanup:
    gss_release_oid(&tmpMinor, &oid);
    gss_release_buffer(&tmpMinor, &assertionBuf);
    gss_release_buffer(&tmpMinor, &nameBuf);

    return major;
}

#endif /* __APPLE__ */
