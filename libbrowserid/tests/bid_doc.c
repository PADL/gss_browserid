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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "browserid.h"
#include "bid_private.h"

/*
 * Test authority document retrieval.
 */
int main(int argc, char *argv[])
{
    BIDError err;
    BIDContext context = NULL;
    BIDAuthority authority = NULL;
    BIDJWK pkey = NULL;
    const char *s;

    err = BIDAcquireContext(NULL, BID_CONTEXT_RP | BID_CONTEXT_VERIFY_REMOTE, NULL, &context);
    BID_BAIL_ON_ERROR(err);

    err = _BIDAcquireAuthority(context, "login.persona.org", time(NULL), &authority);
    BID_BAIL_ON_ERROR(err);

    err = _BIDGetAuthorityPublicKey(context, authority, &pkey);
    BID_BAIL_ON_ERROR(err);

    err = _BIDIssuerIsAuthoritative(context, "padl.com", "login.persona.org", time(NULL));
    BID_BAIL_ON_ERROR(err);

cleanup:
    json_decref(pkey);
    _BIDReleaseAuthority(context, authority);
    BIDReleaseContext(context);

    if (err) {
        BIDErrorToString(err, &s);
        fprintf(stderr, "Error %d %s\n", err, s);
    }

    exit(err);
}
