/*
 * Copyright (c) 2010, JANET(UK)
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
 * Copyright 2001-2009 Internet2
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gssapiP_eap.h"

#include <shibsp/Application.h>
#include <shibsp/exceptions.h>
#include <shibsp/SPConfig.h>
#include <shibsp/ServiceProvider.h>
#include <shibsp/attribute/Attribute.h>
#include <shibsp/attribute/SimpleAttribute.h>
#include <shibsp/attribute/resolver/ResolutionContext.h>
#include <shibsp/handler/AssertionConsumerService.h>
#include <shibsp/metadata/MetadataProviderCriteria.h>
#include <shibsp/util/SPConstants.h>

#include <saml/saml1/core/Assertions.h>
#include <saml/saml2/core/Assertions.h>
#include <saml/saml2/metadata/Metadata.h>
#include <xercesc/util/XMLUniDefs.hpp>
#include <xmltooling/XMLToolingConfig.h>
#include <xmltooling/util/XMLHelper.h>

#include "resolver.h"

using namespace shibsp;
using namespace shibresolver;
using namespace opensaml::saml2md;
using namespace opensaml;
using namespace xmltooling::logging;
using namespace xmltooling;
using namespace xercesc;
using namespace std;

static vector <Attribute *>
duplicateAttributes(const vector <Attribute *>src);

gss_eap_shib_attr_provider::gss_eap_shib_attr_provider(const gss_eap_attr_ctx *ctx,
                                                       gss_cred_id_t gssCred,
                                                       gss_ctx_id_t gssCtx)
    : gss_eap_attr_provider(ctx, gssCred, gssCtx)
{
    if (gssCtx == GSS_C_NO_CONTEXT) {
        gss_eap_shib_attr_provider *shib;

        shib = dynamic_cast<gss_eap_shib_attr_provider *>
            (ctx->getProvider(ATTR_TYPE_LOCAL));
        if (shib != NULL)
            m_attributes = duplicateAttributes(shib->m_attributes);
    } else {
        gss_eap_saml_assertion_provider *saml;
        gss_eap_radius_attr_provider *radius;
        gss_buffer_desc nameBuf = GSS_C_EMPTY_BUFFER;
        ShibbolethResolver *resolver = NULL;
        OM_uint32 minor;

        saml = dynamic_cast<gss_eap_saml_assertion_provider *>
            (ctx->getProvider(ATTR_TYPE_SAML_ASSERTION));
        radius = dynamic_cast<gss_eap_radius_attr_provider *>
            (ctx->getProvider(ATTR_TYPE_RADIUS));

        if (radius == NULL)
            return;

        if (gssCred != GSS_C_NO_CREDENTIAL &&
            gss_display_name(&minor, gssCred->name, &nameBuf, NULL) == GSS_S_COMPLETE)
            resolver->setApplicationID((const char *)nameBuf.value);

        if (saml != NULL && saml->getAssertion() != NULL)
            resolver->addToken(saml->getAssertion());

        /* TODO inject RADIUS attribute types */

        resolver->resolveAttributes(m_attributes);

        gss_release_buffer(&minor, &nameBuf);

        delete resolver;
    }
}

gss_eap_shib_attr_provider::~gss_eap_shib_attr_provider(void)
{
    for_each(m_attributes.begin(), m_attributes.end(), xmltooling::cleanup<Attribute>());
}

int
gss_eap_shib_attr_provider::getAttributeIndex(const gss_buffer_t attr) const
{
    int i = 0;

    for (vector<Attribute *>::const_iterator a = m_attributes.begin();
         a != m_attributes.end();
         ++a)
    {
        for (vector<string>::const_iterator s = (*a)->getAliases().begin();
             s != (*a)->getAliases().end();
             ++s) {
            if (attr->length == (*s).length() &&
                memcmp((*s).c_str(), attr->value, attr->length) == 0) {
                return i;
            }
        }
    }

    return -1;
}

void
gss_eap_shib_attr_provider::setAttribute(int complete,
                                         const gss_buffer_t attr,
                                         const gss_buffer_t value)
{
    string attrStr((char *)attr->value, attr->length);
    vector <string> ids(1);

    ids.push_back(attrStr);

    SimpleAttribute *a = new SimpleAttribute(ids);

    if (value->length != 0) {
        string valStr((char *)value->value, value->length);

        a->getValues().push_back(valStr);        
    }

    m_attributes.push_back(a);
}

void
gss_eap_shib_attr_provider::deleteAttribute(const gss_buffer_t attr)
{
    int i;

    i = getAttributeIndex(attr);
    if (i >= 0)
        m_attributes.erase(m_attributes.begin() + i);
}

bool
gss_eap_shib_attr_provider::getAttributeTypes(gss_eap_attr_enumeration_cb addAttribute,
                                              void *data) const
{
    for (vector<Attribute*>::const_iterator a = m_attributes.begin();
        a != m_attributes.end();
        ++a)
    {
        gss_buffer_desc attribute;

        attribute.value = (void *)((*a)->getId());
        attribute.length = strlen((char *)attribute.value);

        if (!addAttribute(this, &attribute, data))
            return false;
    }

    return true;
}

const Attribute *
gss_eap_shib_attr_provider::getAttribute(const gss_buffer_t attr) const
{
    const Attribute *ret = NULL;

    for (vector<Attribute *>::const_iterator a = m_attributes.begin();
         a != m_attributes.end();
         ++a)
    {
        for (vector<string>::const_iterator s = (*a)->getAliases().begin();
             s != (*a)->getAliases().end();
             ++s) {
            if (attr->length == (*s).length() &&
                memcmp((*s).c_str(), attr->value, attr->length) == 0) {
                ret = *a;
                break;
            }
        }
        if (ret != NULL)
            break;
    }

    return ret;
}

bool
gss_eap_shib_attr_provider::getAttribute(const gss_buffer_t attr,
                                         int *authenticated,
                                         int *complete,
                                         gss_buffer_t value,
                                         gss_buffer_t display_value,
                                         int *more) const
{
    const Attribute *shibAttr = NULL;
    gss_buffer_desc buf;

    shibAttr = getAttribute(attr);
    if (shibAttr == NULL)
        return false;

    if (*more == -1) {
        *more = 0;
    } else if (*more >= (int)shibAttr->valueCount()) {
        *more = 0;
        return true;
    }

    buf.value = (void *)shibAttr->getString(*more);
    buf.length = strlen((char *)buf.value);

    duplicateBuffer(buf, value);
 
    *authenticated = TRUE;
    *complete = FALSE;

    return true;
}

gss_any_t
gss_eap_shib_attr_provider::mapToAny(int authenticated,
                                     gss_buffer_t type_id) const
{
    gss_any_t output;

    vector <Attribute *>v = duplicateAttributes(m_attributes);

    output = (gss_any_t)new vector <Attribute *>(v);

    return output;
}

void
gss_eap_shib_attr_provider::releaseAnyNameMapping(gss_buffer_t type_id,
                                                  gss_any_t input) const
{
    vector <Attribute *> *v = ((vector <Attribute *> *)input);
    delete v;
}

void
gss_eap_shib_attr_provider::marshall(gss_buffer_t buffer) const
{
}

bool
gss_eap_shib_attr_provider::unmarshall(const gss_eap_attr_ctx *ctx,
                                       const gss_buffer_t buffer)
{
    return false;
}

bool
gss_eap_shib_attr_provider::init(void)
{
    return ShibbolethResolver::init();
}

void
gss_eap_shib_attr_provider::finalize(void)
{
    ShibbolethResolver::term();
}

gss_eap_attr_provider *
gss_eap_shib_attr_provider::createAttrContext(const gss_eap_attr_ctx *ctx,
                                              gss_cred_id_t gssCred,
                                              gss_ctx_id_t gssCtx)
{
    return new gss_eap_shib_attr_provider(ctx, gssCred, gssCtx);
}

static Attribute *
duplicateAttribute(const Attribute *src)
{
    Attribute *attribute;

    DDF obj = src->marshall();
    attribute = Attribute::unmarshall(obj);
    obj.destroy();

    return attribute;
}

static vector <Attribute *>
duplicateAttributes(const vector <Attribute *>src)
{
    vector <Attribute *> dst;

    for (vector<Attribute *>::const_iterator a = src.begin();
         a != src.end();
         ++a)
        dst.push_back(duplicateAttribute(*a));

    return dst;
}
