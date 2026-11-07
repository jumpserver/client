#include "protocol_events.h"
#import <Foundation/Foundation.h>
#import <CoreServices/CoreServices.h>
#import <ApplicationServices/ApplicationServices.h>

namespace {
std::function<void(QString)> urlReceiver;
}

@interface ClientQtProtocolEventReceiver : NSObject
- (void)receiveURL:(NSAppleEventDescriptor *)event reply:(NSAppleEventDescriptor *)reply;
@end

@implementation ClientQtProtocolEventReceiver
- (void)receiveURL:(NSAppleEventDescriptor *)event reply:(NSAppleEventDescriptor *)reply {
    Q_UNUSED(reply);
    NSString *raw = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];
    if (raw == nil || !urlReceiver) return;
    // Read the original Apple event string, before Qt parses the URL authority.
    const QString url = QString::fromUtf8([raw UTF8String]);
    if (url.startsWith(QStringLiteral("jms://")) && url.size() <= 1024 * 1024)
        urlReceiver(url);
}
@end

void installProtocolEventHandler(std::function<void(QString)> handler) {
    static ClientQtProtocolEventReceiver *receiver = nil;
    NSAppleEventManager *manager = [NSAppleEventManager sharedAppleEventManager];
    if (!handler) {
        [manager removeEventHandlerForEventClass:kInternetEventClass andEventID:kAEGetURL];
        urlReceiver = {};
#if !__has_feature(objc_arc)
        [receiver release];
#endif
        receiver = nil;
        return;
    }
    urlReceiver = std::move(handler);
    if (receiver == nil) receiver = [[ClientQtProtocolEventReceiver alloc] init];
    [manager setEventHandler:receiver andSelector:@selector(receiveURL:reply:)
               forEventClass:kInternetEventClass andEventID:kAEGetURL];
}
