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
 *    how to obtain complete source code for the gss_browserid software
 *    and any accompanying software that uses the gss_browserid software.
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

#include <AppKit/AppKit.h>
#include <WebKit/WebKit.h>

#include "bid_private.h"
#include "bid_json.h"

@interface BIDIdentityDialog : NSPanel
+ (BIDIdentityDialog *)identityDialog;
@end

@implementation BIDIdentityDialog
+ (BIDIdentityDialog *)identityDialog
{
    return [[self alloc] init];
}

- (id)init
{
    NSRect frame = NSMakeRect(0, 0, 700, 375);
    NSUInteger styleMask = NSTitledWindowMask | NSClosableWindowMask | NSUtilityWindowMask;
    NSRect rect = [NSPanel contentRectForFrameRect:frame styleMask:styleMask];

    self = [super initWithContentRect:rect styleMask:styleMask backing:NSBackingStoreBuffered defer:YES];

    self.hidesOnDeactivate = YES;
    self.worksWhenModal = YES;

    return self;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

- (BOOL)canBecomeMainWindow
{
    return YES;
}
@end

@interface BIDIdentityController : NSObject <NSWindowDelegate>

@property(nonatomic, copy) NSString *audience;
@property(nonatomic, retain) BIDJsonDictionary *claims;
@property(nonatomic, copy) NSString *emailHint;
@property(nonatomic, copy) NSString *siteName;
@property(nonatomic, retain, readonly) NSString *assertion;
@property(nonatomic, assign) BOOL canInteract;
@property(nonatomic, assign) BOOL silent;
@property(nonatomic, retain, readonly) NSWindow *parentWindow;
@property(nonatomic, readonly) BIDError bidError;
@property(nonatomic, retain, readonly) BIDIdentityDialog *identityDialog;

/* helpers */
- (void)closeIdentityDialog;
- (void)abortWithError:(NSError *)error;
- (void)identityCallback:(NSString *)assertion withParameters:(id)parameters;
- (void)interposeAssertionSign:(WebView *)sender;
- (void)acquireAssertion:(WebView *)webView;
- (WebView *)newWebView;

/* public interface */
- (BIDError)getAssertion;
- (id)initWithAudience:(NSString *)anAudience claims:(BIDJsonDictionary *)someClaims;
@end

@interface BIDIdentityController ()
@property(nonatomic, retain, readwrite) NSString *assertion;
@property(nonatomic, retain, readwrite) NSWindow *parentWindow;
@property(nonatomic, readwrite) BIDError bidError;
@property(nonatomic, retain, readwrite) BIDIdentityDialog *identityDialog;
@property(nonatomic, retain, readwrite) WebView *webView;
@end

@implementation BIDIdentityController
{
    NSString *_audience;
}

#pragma mark - accessors

- (NSString *)audience
{
    return _audience;
}

- (void)setAudience:(NSString *)value
{
    if (value != _audience) {
        NSArray *princComponents;

        _audience = [value copy];

        princComponents = [_audience componentsSeparatedByString:@"/"];
        if ([princComponents count] > 1)
            self.siteName = [princComponents objectAtIndex:1];
        else
            self.siteName = _audience;
    }
}

#pragma mark - helpers

- (WebView *)newWebView
{
    NSRect frame = NSMakeRect(0, 0, 700, 375);
    WebView *aWebView = [[WebView alloc] initWithFrame:frame];

    if (aWebView != nil) {
        aWebView.frameLoadDelegate = self;
        aWebView.resourceLoadDelegate = self;
        aWebView.UIDelegate = self;
        aWebView.policyDelegate = self;
        aWebView.hostWindow = self.identityDialog;
        aWebView.shouldCloseWithWindow = YES;
    }

    return aWebView;
}

- (void)closeIdentityDialog
{
    [self.identityDialog close];
}

- (void)abortWithError:(NSError *)error
{
    if (error != nil &&
        ([[error domain] isEqualToString:NSURLErrorDomain] ||
         [[error domain] isEqualToString:WebKitErrorDomain]))
        self.bidError = BID_S_HTTP_ERROR;
    else
        self.bidError = BID_S_INTERACT_FAILURE;

    [self closeIdentityDialog];
}

#pragma mark - javascript methods

- (void)identityCallback:(NSString *)anAssertion withParameters:(id)BID_UNUSED parameters
{
    if (anAssertion != nil)
        self.bidError = BID_S_OK;
    else if (self.silent)
        self.bidError = BID_S_INTERACT_REQUIRED;
    else
        self.bidError = BID_S_INTERACT_FAILURE;

    if (self.bidError == BID_S_INTERACT_REQUIRED && self.canInteract) {
        self.silent = NO;
        [self acquireAssertion:self.webView];
    } else {
        self.assertion = anAssertion;
        [self closeIdentityDialog];
    }
}

#pragma mark - delegates

+ (BOOL)isKeyExcludedFromWebScript:(const char *)property
{
    if (strcmp(property, "_siteName") == 0              ||
        strcmp(property, "_claims") == 0                ||
        strcmp(property, "_silent") == 0                ||
        strcmp(property, "_emailHint") == 0             ||
        strcmp(property, "_audience") == 0)
        return NO;

    return YES;
}

+ (BOOL)isSelectorExcludedFromWebScript:(SEL)selector
{
    if (selector == @selector(identityCallback:withParameters:))
        return NO;

    return YES;
}

- (void)windowWillClose:(NSNotification *)BID_UNUSED notification
{
    [NSApp stopModal];
}

- (void)interposeAssertionSign:(WebView *)sender
{
    NSString *function = @"                                                                             \
        var controller = window.IdentityController;                                                     \
        var oldLoad = BrowserID.CryptoLoader.load;                                                      \
                                                                                                        \
        BrowserID.CryptoLoader.load = function(onSuccess, onFailure) {                                  \
            oldLoad(function(jwCrypto) {                                                                \
                var assertionSign = jwCrypto.assertion.sign;                                            \
                                                                                                        \
                jwCrypto.assertion.sign = function(payload, assertionParams, secretKey, cb) {           \
                    var gssPayload = JSON.parse(controller._claims.jsonRepresentation());               \
                    for (var k in payload) {                                                            \
                        if (payload.hasOwnProperty(k)) gssPayload[k] = payload[k];                      \
                    }                                                                                   \
                    assertionSign(gssPayload, assertionParams, secretKey, cb);                          \
                };                                                                                      \
                onSuccess(jwCrypto);                                                                    \
            }, onFailure);                                                                              \
        };                                                                                              \
    ";

    [sender stringByEvaluatingJavaScriptFromString:function];
}

- (void)acquireAssertion:(WebView *)sender
{
    NSString *function = @"                                                                             \
        var controller = window.IdentityController;                                                     \
        var options = { siteName: controller._siteName, silent: controller._silent,                     \
                        experimental_emailHint: controller._emailHint };                                \
                                                                                                        \
        BrowserID.internal.get(                                                                         \
            controller._audience,                                                                       \
            function(assertion, params) {                                                               \
                controller.identityCallback_withParameters_(assertion, params);                         \
            },                                                                                          \
            options);                                                                                   \
    ";

    [sender stringByEvaluatingJavaScriptFromString:function];

    if (!self.silent) {
        [self.identityDialog makeFirstResponder:sender];
        self.identityDialog.contentView = sender;
        [self.identityDialog makeKeyAndOrderFront:sender];
        [self.identityDialog center];
    }
}

- (void)webView:(WebView *)BID_UNUSED webView addMessageToConsole:(NSDictionary *)message
{
    NSLog(@"%@", message);
}

- (void)webView:(WebView *)sender didFinishLoadForFrame:(WebFrame *)frame
{
    if ([sender isEqual:self.webView] && frame == [sender mainFrame]) {
        if (self.claims.count)
            [self interposeAssertionSign:sender];
        [self acquireAssertion:sender];
    }
}

- (void)webView:(WebView *)BID_UNUSED sender windowScriptObjectAvailable:(WebScriptObject *)windowScriptObject
{
    [windowScriptObject setValue:self forKey:@"IdentityController"];
}

- (void)webView:(WebView *)sender didFailLoadWithError:(NSError *)error forFrame:(WebFrame *)frame
{
    NSLog(@"webView:%@ didFailLoadWithError:%@ forFrame:%@", [sender description], [error description], [frame name]);
    if (error.code == NSURLErrorCancelled)
        return;
    else
        [self abortWithError:error];
}

- (void)webView:(WebView *)sender didFailProvisionalLoadWithError:(NSError *)error forFrame:(WebFrame *)frame
{
    NSLog(@"webView:%@ didFailProvisionalLoadWithError:%@ forFrame:%@", [sender description], [error description], [frame name]);
    [self abortWithError:error];
}

- (void)webView:(WebView *)BID_UNUSED sender decidePolicyForNavigationAction:(NSDictionary *)BID_UNUSED actionInformation request:(NSURLRequest *)BID_UNUSED request frame:(WebFrame *)BID_UNUSED frame decisionListener:(id<WebPolicyDecisionListener>)listener
{
#if 0
    NSLog(@"webView:%@ decidePolicyForNavigationAction:%@ request:%@ frame:%@ decisionListener:%@", sender, [actionInformation objectForKey:WebActionOriginalURLKey], request, [frame name], listener);
#endif
    [listener use];
}

- (void)webView:(WebView *)sender decidePolicyForNewWindowAction:(NSDictionary *)actionInformation request:(NSURLRequest *)request newFrameName:(NSString *)frameName decisionListener:(id<WebPolicyDecisionListener>)listener
{
    NSLog(@"webView:%@ decidePolicyForNewWindowAction:%@ request:%@ frame:%@", sender, [actionInformation objectForKey:WebActionOriginalURLKey], request, frameName);
    if ([actionInformation objectForKey:WebActionElementKey]) {
        [listener ignore];
        [[NSWorkspace sharedWorkspace] openURL:[request URL]];
    } else {
        [listener use];
    }
}

#if 0
- (WebView *)webView:(WebView *)sender createWebViewWithRequest:(NSURLRequest *)request
{
    WebView *aWebView = [self newWebView];

    NSLog(@"createWebViewWithRequest %@", request);
    self.identityDialog.contentView = aWebView;

    return aWebView;
}
#endif

#pragma mark - public
- (id)init
{
    self.bidError = BID_S_INTERACT_FAILURE;

    return [super init];
}

- (id)initWithAudience:(NSString *)anAudience claims:(BIDJsonDictionary *)someClaims
{
    self = [self init];

    if (self != nil) {
        self.audience = anAudience;
        self.claims = someClaims;
    }

    return self;
}

- (BIDError)getAssertion
{
    NSApplication *app = [NSApplication sharedApplication];
    NSURL *personaURL = [NSURL URLWithString:@BID_SIGN_IN_URL];

    if (self.audience == nil)
        return (self.bidError = BID_S_INVALID_AUDIENCE_URN);

    if (self.canInteract == NO && self.silent == NO)
        return (self.bidError = BID_S_INTERACT_REQUIRED);

    self.identityDialog = [BIDIdentityDialog identityDialog];
    self.identityDialog.delegate = self;
    if (self.silent)
        [self.identityDialog orderOut:nil];
    if (self.parentWindow != nil)
        self.identityDialog.parentWindow = self.parentWindow;

    self.webView = [self newWebView];
    [[self.webView mainFrame] loadRequest:[NSURLRequest requestWithURL:personaURL]];

    [app runModalForWindow:self.identityDialog];

    return self.bidError;
}

@end

BIDError
_BIDBrowserGetAssertion(
    BIDContext context,
    const char *szAudienceOrSpn,
    json_t *claims,
    const char *szIdentityName,
    uint32_t ulReqFlags,
    char **pAssertion)
{
    BIDError err = BID_S_INTERACT_FAILURE;
    BIDIdentityController *controller = nil;

    *pAssertion = NULL;

#ifdef GSSBID_DEBUG
    /*
     * Only applications that are NSApplicationActivationPolicyRegular or
     * NSApplicationActivationPolicyAccessory can interact with the user.
     * Don't try to show UI if this is not the case, unless building with
     * compile time debugging.
     */
    if ([NSApp activationPolicy] == NSApplicationActivationPolicyProhibited ||
        ![NSApp isRunning]) {
        ProcessSerialNumber psn = { 0, kCurrentProcess };
        TransformProcessType(&psn, kProcessTransformToUIElementApplication);
    }
#endif /* GSSBID_DEBUG */

    if ([NSApp activationPolicy] == NSApplicationActivationPolicyProhibited ||
        !NSApplicationLoad())
        return BID_S_INTERACT_UNAVAILABLE;

    @autoreleasepool {
        BIDJsonDictionary *claimsDict = [[BIDJsonDictionary alloc] initWithJsonObject:claims];

        controller = [[BIDIdentityController alloc] initWithAudience:[NSString stringWithUTF8String:szAudienceOrSpn] claims:claimsDict];
        if (szIdentityName != NULL) {
            controller.emailHint = [NSString stringWithUTF8String:szIdentityName];
            controller.silent = !!(context->ContextOptions & BID_CONTEXT_BROWSER_SILENT);
        }
        if (context->ParentWindow != NULL)
            controller.parentWindow = (__bridge NSWindow *)context->ParentWindow;
        controller.canInteract = _BIDCanInteractP(context, ulReqFlags);
        [controller performSelectorOnMainThread:@selector(getAssertion) withObject:nil waitUntilDone:TRUE];

        err = controller.bidError;
        if (err == BID_S_OK)
            err = _BIDDuplicateString(context, [[controller assertion] cString], pAssertion);
    }

    return err;
}
