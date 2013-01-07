# BrowserID GSS Mechanism

The BrowserID GSS mechanism is a plugin for the GSS-API that allows you to use
BrowserID-generated assertions for signing in to non-web protocols, such as
SMTP, IMAP, SSH, LDAP, CIFS, and NFS. Any protocol that supports GSS-API or
SASL and does not require mutual authentication should work.

More information on BrowserID is available at the URL
<https://developer.mozilla.org/en-US/docs/persona>.

## Building

The following packages are required:

* Jansson <http://www.digip.org/jansson/>
* OpenSSL <http://www.openssl.org/>
* Kerberos 5 <http://web.mit.edu/kerberos/> or <http://www.h5l.org/>
* Curl <http://curl.haxx.se/>
* Currently, a recent Mac OS X system, as the embedded browser only supports WebKit

Optional:

* OpenSAML <https://wiki.shibboleth.net/confluence/display/OpenSAML/Home>
* Shibboleth <http://shibboleth.net/>

## Installation

Build similar to the following:

    ./autogen.sh
    OBJC=clang CC=clang CXX=clang++ ./configure

Edit /usr/local/etc/gss/mech (replace path as appropriate) and add the
following mechanisms, updating the path as appropriate:

    browserid-aes128    1.3.6.1.4.1.5322.24.1.17 /usr/local/lib/gss/mech_browserid.so

## Testing

### gss-sample

gss-sample can be found in src/appl/gss-sample in the MIT Kerberos
distribution.

    % gss-client -port 5555 -mech "{1 3 6 1 4 1 5322 24 1 17}" localhost host "Testing GSS BrowserID"
    % gss-server -port 5555 -export host

If you are testing between different machines, then you should do (replacing
server as appropriate)

    % gss-client -port 5555 -mech "{1 3 6 1 4 1 5322 24 1 17}" server host@server "Testing GSS BrowserID"
    % gss-server -port 5555 -export host@server

Note that if you test the browserid-none (no key) mechanism than the message
protection tests will fail.

### SASL

SASL samples can be found in the sample directory of the Cyrus SASL
distribution. However, the GS2 mechanism presently needs a patch to support
mechanisms without mutual authentication (this can be found in contrib).

    % client -C -p 5556 -s host -m BROWSERID-AES128 server
    % server -c -p 5556 -s host -h server
