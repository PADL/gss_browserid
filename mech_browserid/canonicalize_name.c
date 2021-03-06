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
 * Function for canonicalizing a name; presently just duplicates it.
 */

#include "gssapiP_bid.h"

OM_uint32 GSSAPI_CALLCONV
gss_canonicalize_name(OM_uint32 *minor,
#ifdef HAVE_HEIMDAL_VERSION
                      gss_const_name_t input_name_const,
                      const gss_OID mech_type,
#else
                      const gss_name_t input_name,
                      const gss_OID mech_type,
#endif
                      gss_name_t *output_name)
{
    OM_uint32 major;
#ifdef HAVE_HEIMDAL_VERSION
    const gss_name_t input_name = (const gss_name_t)input_name_const;
#endif

    *minor = 0;

    if (!gssBidIsMechanismOid(mech_type))
        return GSS_S_BAD_MECH;

    if (input_name == GSS_C_NO_NAME) {
        *minor = EINVAL;
        return GSS_S_CALL_INACCESSIBLE_READ | GSS_S_BAD_NAME;
    }

    GSSBID_MUTEX_LOCK(&input_name->mutex);

    major = gssBidCanonicalizeName(minor, input_name, mech_type, output_name);

    GSSBID_MUTEX_UNLOCK(&input_name->mutex);

    return major;
}
