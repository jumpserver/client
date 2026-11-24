#include "protocol_events.h"
#include "protocollog.h"
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <ApplicationServices/ApplicationServices.h>

namespace {
std::function<void(QString)> urlReceiver;
}

@interface ClientQtProtocolEventReceiver : NSObject
- (void)receiveURL:(NSAppleEventDescriptor *)event reply:(NSAppleEventDescriptor *)reply;
- (void)applicationWillFinishLaunching:(NSNotification *)notification;
@end

@implementation ClientQtProtocolEventReceiver
- (void)applicationWillFinishLaunching:(NSNotification *)notification {
    Q_UNUSED(notification);
    [[NSAppleEventManager sharedAppleEventManager] setEventHandler:self
        andSelector:@selector(receiveURL:reply:)
        forEventClass:kInternetEventClass andEventID:kAEGetURL];
    protocolLog("macOS URL AppleEvent handler installed after Qt startup");
}
- (void)receiveURL:(NSAppleEventDescriptor *)event reply:(NSAppleEventDescriptor *)reply {
    protocolLog("macOS URL AppleEvent received");
    Q_UNUSED(reply);
    NSString *raw = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];
    if (raw == nil || !urlReceiver) { protocolLog("AppleEvent missing URL or receiver"); return; }
    // Read the original Apple event string, before Qt parses the URL authority.
    const QString url = QString::fromUtf8([raw UTF8String]);
    if (url.startsWith(QStringLiteral("jms://")) && url.size() <= 1024 * 1024)
        urlReceiver(url);
    else
        protocolLog("AppleEvent URL rejected: scheme or length");
}
@end

void installProtocolEventHandler(std::function<void(QString)> handler) {
    static ClientQtProtocolEventReceiver *receiver = nil;
    NSAppleEventManager *manager = [NSAppleEventManager sharedAppleEventManager];
    if (!handler) {
        [[NSNotificationCenter defaultCenter] removeObserver:receiver];
        [manager removeEventHandlerForEventClass:kInternetEventClass andEventID:kAEGetURL];
        urlReceiver = {};
#if !__has_feature(objc_arc)
        [receiver release];
#endif
        receiver = nil;
        return;
    }
    urlReceiver = std::move(handler);
    if (receiver == nil) {
        receiver = [[ClientQtProtocolEventReceiver alloc] init];
        [[NSNotificationCenter defaultCenter] addObserver:receiver
            selector:@selector(applicationWillFinishLaunching:)
            name:NSApplicationWillFinishLaunchingNotification object:nil];
    }
    [manager setEventHandler:receiver andSelector:@selector(receiveURL:reply:)
               forEventClass:kInternetEventClass andEventID:kAEGetURL];
    protocolLog("macOS URL AppleEvent handler installed");
}
