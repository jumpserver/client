#include "protocol_events.h"
#include <QCoreApplication>
#include <QDebug>
#import <Foundation/Foundation.h>
#import <ApplicationServices/ApplicationServices.h>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    @autoreleasepool {
        QString received;
        installProtocolEventHandler([&received](QString raw) { received = raw; });
        const QString raw = "jms://" + QString::fromLatin1(QByteArray(
            "{\"protocol\":\"ssh\",\"host\":\"127.0.0.1\",\"username\":\"synthetic\"}").toBase64());
        NSAppleEventDescriptor *event = [NSAppleEventDescriptor
            appleEventWithEventClass:kInternetEventClass eventID:kAEGetURL
            targetDescriptor:nil returnID:kAutoGenerateReturnID transactionID:kAnyTransactionID];
        [event setParamDescriptor:[NSAppleEventDescriptor descriptorWithString:
            [NSString stringWithUTF8String:raw.toUtf8().constData()]] forKeyword:keyDirectObject];
        AppleEvent reply = {typeNull, nullptr};
        const OSErr status = [[NSAppleEventManager sharedAppleEventManager]
            dispatchRawAppleEvent:[event aeDesc] withRawReply:&reply handlerRefCon:&received];
        AEDisposeDesc(&reply);
        installProtocolEventHandler({});
        if (status != noErr || received != raw) {
            qCritical("Native protocol event did not preserve the original payload");
            return 1;
        }
        qInfo("Synthetic native protocol event preserved the original payload");
    }
    return 0;
}
