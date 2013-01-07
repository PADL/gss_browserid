/*
 * Copyright (C) 2013 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 */

#include "bid_private.h"

/*
 * From https://github.com/mozilla/id-specs/blob/prod/browserid/index.md:
 *
 * If the exp date of the assertion is earlier than the current time by more
 * a certain interval, the assertion has expired and must be rejected. A
 * Party MAY choose the length of that interval, though it is recommended
 * it be less than 5 minutes.
 */
BIDError
_BIDValidateExpiry(
    BIDContext context,
    time_t verificationTime,
    json_t *jwt)
{
    BIDError err = BID_S_OK;
    time_t issueTime = 0, notBefore = 0, expiryTime = 0;

    err = _BIDGetJsonTimestampValue(context, jwt, "iat", &issueTime);
    if (err == BID_S_OK && issueTime - verificationTime > context->Skew) {
        err = BID_S_INVALID_ASSERTION;
        goto cleanup;
    }

    err = _BIDGetJsonTimestampValue(context, jwt, "nbf", &notBefore);
    if (err == BID_S_OK && notBefore - verificationTime > context->Skew) {
        err = BID_S_ASSERTION_NOT_YET_VALID;
        goto cleanup;
    }

    err = _BIDGetJsonTimestampValue(context, jwt, "exp", &expiryTime);
    if (err == BID_S_UNKNOWN_JSON_KEY && issueTime != 0) {
        /* XXX use Skew as default lifetime as well as clock skew */
        expiryTime = issueTime + context->Skew;
        err = BID_S_OK;
    }
    BID_BAIL_ON_ERROR(err);

    if (verificationTime - expiryTime > context->Skew) {
        err = BID_S_EXPIRED_ASSERTION;
        goto cleanup;
    }

cleanup:
    return err;
}

/*
 * From https://github.com/mozilla/id-specs/blob/prod/browserid/index.md:
 *
 * If the audience field of the assertion does not match the Relying Party's
 * origin (including scheme and optional non-standard port), reject the assertion.
 * A domain that includes the standard port, of 80 for HTTP and 443 for HTTPS,
 * SHOULD be treated as equivalent to a domain that matches the protocol but does
 * not include the port. (XXX: Can we find an RFC that defines this equality
 * test?)
 */
BIDError
_BIDValidateAudience(
    BIDContext context,
    BIDBackedAssertion backedAssertion,
    const char *szAudienceOrSpn,
    const unsigned char *pbChannelBindings,
    size_t cbChannelBindings)
{
    BIDError err;
    unsigned char *pbAssertionCB = NULL;
    size_t cbAssertionCB = 0;
    json_t *claims = backedAssertion->Assertion->Payload;

    if (claims == NULL)
        return BID_S_MISSING_AUDIENCE;

    if (szAudienceOrSpn != NULL) {
        const char *szAssertionSpn = json_string_value(json_object_get(claims, "aud"));

        if (szAssertionSpn == NULL) {
            err = BID_S_MISSING_AUDIENCE;
            goto cleanup;
        } else if (strcmp(szAudienceOrSpn, szAssertionSpn) != 0) {
            err = BID_S_BAD_AUDIENCE;
            goto cleanup;
        }
    }

    if (pbChannelBindings != NULL) {
        err = _BIDGetJsonBinaryValue(context, claims, "cbt", &pbAssertionCB, &cbAssertionCB);
        if (err == BID_S_UNKNOWN_JSON_KEY)
            err = BID_S_MISSING_CHANNEL_BINDINGS;
        BID_BAIL_ON_ERROR(err);

        if (cbChannelBindings != cbAssertionCB ||
            memcmp(pbChannelBindings, pbAssertionCB, cbAssertionCB) != 0) {
            err = BID_S_CHANNEL_BINDINGS_MISMATCH;
            goto cleanup;
        }
    }

    err = BID_S_OK;

cleanup:
    BIDFree(pbAssertionCB);

    return err;
}

/*
 * From https://github.com/mozilla/id-specs/blob/prod/browserid/index.md:
 *
 * If the Identity Assertion's signature does not verify against the
 * public-key within the last Identity Certificate, reject the assertion.
 */
static BIDError
_BIDVerifyAssertionSignature(
    BIDContext context,
    BIDBackedAssertion backedAssertion,
    BIDJWK reauthCred)
{
    BIDJWK verifyCred;

    if (backedAssertion->cCertificates == 0) {
        BID_ASSERT(reauthCred != NULL);

        verifyCred = reauthCred;
    } else {
        verifyCred = backedAssertion->rCertificates[backedAssertion->cCertificates - 1]->Payload;
    }

    return _BIDVerifySignature(context, backedAssertion->Assertion, verifyCred);
}

/*
 * From https://github.com/mozilla/id-specs/blob/prod/browserid/index.md:
 *
 * If the first certificate (or only certificate when
 * there is only one) is not properly signed by the expected issuer's public key,
 * reject the assertion. The expected issuer is either the domain of the certified
 * email address in the last certificate, or the issuer listed in the first
 * certificate if the email-address domain does not support BrowserID.
 */
static BIDError
_BIDValidateCertIssuer(
    BIDContext context,
    BIDBackedAssertion backedAssertion)
{
    BIDError err;
    json_t *assertion;
    json_t *leafCert;
    json_t *principal;
    const char *szEmail;
    const char *szEmailIssuer;
    const char *szCertIssuer;

    if (backedAssertion->cCertificates == 0)
        return BID_S_MISSING_CERT;

    leafCert = _BIDLeafCert(context, backedAssertion);
    assertion = backedAssertion->Assertion->Payload;

    principal = json_object_get(leafCert, "principal");
    if (principal == NULL)
        return BID_S_MISSING_PRINCIPAL;

    szEmail = json_string_value(json_object_get(principal, "email"));
    if (szEmail == NULL)
        return BID_S_UNKNOWN_PRINCIPAL_TYPE;

    szEmailIssuer = strchr(szEmail, '@');
    if (szEmailIssuer == NULL)
        return BID_S_INVALID_ISSUER;

    szEmailIssuer++;

    szCertIssuer = json_string_value(json_object_get(leafCert, "iss"));
    if (szCertIssuer == NULL)
        return BID_S_MISSING_ISSUER;

    err = _BIDIssuerIsAuthoritative(context, szEmailIssuer, szCertIssuer);

    return err;
}

/*
 * From https://github.com/mozilla/id-specs/blob/prod/browserid/index.md:
 *
 * If there is more than one Identity Certificate, then reject the assertion
 * unless each certificate after the first one is properly signed by the prior
 * certificate's public key.
 */
static BIDError
_BIDValidateCertChain(
    BIDContext context,
    BIDBackedAssertion backedAssertion,
    time_t verificationTime)
{
    BIDError err;
    BIDAuthority authority = NULL;
    BIDJWKSet rootKey = NULL, pKey = NULL;
    json_t *rootCert = _BIDRootCert(context, backedAssertion);
    const char *szCertIssuer;
    size_t i;

    if (backedAssertion->cCertificates == 0)
        return BID_S_MISSING_CERT;

    szCertIssuer = json_string_value(json_object_get(rootCert, "iss"));
    if (szCertIssuer == NULL) {
        err = BID_S_MISSING_ISSUER;
        goto cleanup;
    }

    err = _BIDAcquireAuthority(context, szCertIssuer, &authority);
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetAuthorityPublicKey(context, authority, &rootKey);
    BID_BAIL_ON_ERROR(err);

    pKey = json_incref(rootKey);

    for (i = 0; i < backedAssertion->cCertificates; i++) {
        BIDJWT cert = backedAssertion->rCertificates[i];
        err = _BIDValidateExpiry(context, verificationTime, cert->Payload);
        BID_BAIL_ON_ERROR(err);

        err = _BIDVerifySignature(context, cert, pKey);
        BID_BAIL_ON_ERROR(err);

        json_decref(pKey);
        pKey = json_incref(cert->Payload);
    }

cleanup:
    switch (err) {
    case BID_S_ASSERTION_NOT_YET_VALID:
        err = BID_S_CERT_NOT_YET_VALID;
        break;
    case BID_S_EXPIRED_ASSERTION:
        err = BID_S_EXPIRED_CERT;
        break;
    default:
        break;
    }

    _BIDReleaseAuthority(context, authority);
    json_decref(rootKey);

    return err;
}

/*
 * Local verifier
 */
BIDError
_BIDVerifyLocal(
    BIDContext context,
    BIDReplayCache replayCache,
    const char *szAssertion,
    const char *szAudience,
    const unsigned char *pbChannelBindings,
    size_t cbChannelBindings,
    time_t verificationTime,
    uint32_t ulReqFlags,
    BIDIdentity *pVerifiedIdentity,
    time_t *pExpiryTime,
    uint32_t *pulRetFlags)
{
    BIDError err;
    BIDBackedAssertion backedAssertion = NULL;
    BIDIdentity verifiedIdentity = BID_C_NO_IDENTITY;
    BIDJWK reauthCred = NULL;

    *pVerifiedIdentity = BID_C_NO_IDENTITY;
    *pExpiryTime = 0;
    *pulRetFlags = 0;

    BID_CONTEXT_VALIDATE(context);

    /*
     * Split backed identity assertion out into
     * <cert-1>~...<cert-n>~<identityAssertion>
     */
    err = _BIDUnpackBackedAssertion(context, szAssertion, &backedAssertion);
    BID_BAIL_ON_ERROR(err);

    BID_ASSERT(backedAssertion->Assertion != NULL);
    BID_ASSERT(backedAssertion->Assertion->Payload != NULL);

    if (backedAssertion->cCertificates == 0) {
        if ((context->ContextOptions & BID_CONTEXT_REAUTH) == 0 ||
            (ulReqFlags & BID_VERIFY_FLAG_NO_REAUTH)) {
            err = BID_S_INVALID_ASSERTION;
            goto cleanup;
        }

        *pulRetFlags |= BID_VERIFY_FLAG_REAUTH;

        err = _BIDVerifyReauthAssertion(context, replayCache,
                                        backedAssertion, verificationTime,
                                        &verifiedIdentity, &reauthCred);
        BID_BAIL_ON_ERROR(err);
    }

    err = _BIDValidateAudience(context, backedAssertion, szAudience, pbChannelBindings, cbChannelBindings);
    BID_BAIL_ON_ERROR(err);

    err = _BIDValidateExpiry(context, verificationTime, backedAssertion->Assertion->Payload);
    BID_BAIL_ON_ERROR(err);

    /* Only allow one certificate for now */
    if (backedAssertion->cCertificates > 1) {
        err = BID_S_TOO_MANY_CERTS;
        goto cleanup;
    }

    if (backedAssertion->cCertificates > 0) {
        err = _BIDValidateCertIssuer(context, backedAssertion);
        BID_BAIL_ON_ERROR(err);

        err = _BIDValidateCertChain(context, backedAssertion, verificationTime);
        BID_BAIL_ON_ERROR(err);
    }

    err = _BIDVerifyAssertionSignature(context, backedAssertion, reauthCred);
    BID_BAIL_ON_ERROR(err);

    if (verifiedIdentity == BID_C_NO_IDENTITY) {
        err = _BIDPopulateIdentity(context, backedAssertion, &verifiedIdentity);
        BID_BAIL_ON_ERROR(err);
    }

    err = BID_S_OK;
    *pVerifiedIdentity = verifiedIdentity;
    _BIDGetJsonTimestampValue(context, verifiedIdentity->Attributes, "exp", pExpiryTime);

cleanup:
    _BIDReleaseBackedAssertion(context, backedAssertion);
    if (err != BID_S_OK)
        BIDReleaseIdentity(context, verifiedIdentity);
    json_decref(reauthCred);
    
    return err;
}
