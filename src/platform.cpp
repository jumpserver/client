#include "platform.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QHostAddress>
#include <QRegularExpression>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QThread>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFProxySupport.h>
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#elif defined(CLIENT_QT_HAS_LIBSECRET)
// GDBusInterfaceInfo has a field named signals, which conflicts with Qt's macro.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")
#endif

namespace {
constexpr qint64 MaximumMessage = 1024 * 1024;
const char *CredentialService = "com.jumpserver.client.auth";
void setError(QString *error, const QString &message) { if (error) *error = message; }
#ifdef Q_OS_MACOS
CFMutableDictionaryRef credentialQuery(const QString &key) {
    auto query = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks);
    const QByteArray utf8 = key.toUtf8();
    auto account = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(utf8.constData()),
                                          utf8.size(), kCFStringEncodingUTF8, false);
    auto service = CFStringCreateWithCString(nullptr, CredentialService, kCFStringEncodingUTF8);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFRelease(account);
    CFRelease(service);
    return query;
}
#elif defined(CLIENT_QT_HAS_LIBSECRET)
const SecretSchema CredentialSchema = {
    "com.jumpserver.client.auth", SECRET_SCHEMA_NONE,
    {{"key", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}
};
#endif
}

namespace {
QRegularExpression wildcardExpression(const QString &pattern)
{
    // Translate the system's wildcard syntax directly instead of relying on
    // QRegExp (removed in Qt 6) so Qt 5 and Qt 6 builds behave the same.
    QString escaped;
    escaped.reserve(pattern.size() * 2);
    for (const QChar ch : pattern) {
        if (ch == QLatin1Char('*'))
            escaped += QStringLiteral(".*");
        else
            escaped += QRegularExpression::escape(QString(ch));
    }
    return QRegularExpression(QStringLiteral("\\A") + escaped + QStringLiteral("\\z"),
                              QRegularExpression::CaseInsensitiveOption);
}
bool hostExcludedImpl(const QString &host, const QStringList &patterns, bool excludeSimpleHostnames)
{
    if (host.isEmpty()) return true;
    if (excludeSimpleHostnames && !host.contains(QLatin1Char('.')) && !host.contains(QLatin1Char(':')))
        return true;
    QHostAddress address;
    const bool isIp = address.setAddress(host);
    for (const QString &rawPattern : patterns) {
        const QString pattern = rawPattern.trimmed();
        if (pattern.isEmpty()) continue;
        if (isIp) {
            // An entry may be a bare IP or a CIDR subnet; both mean "direct".
            const auto subnet = QHostAddress::parseSubnet(pattern);
            if (subnet.first.isNull() || subnet.second < 0) {
                if (address == QHostAddress(pattern)) return true;
            } else if (address.isInSubnet(subnet)) {
                return true;
            }
        }
        if (wildcardExpression(pattern).match(host).hasMatch()) return true;
    }
    return false;
}
#ifdef Q_OS_MACOS
QString cfString(CFDictionaryRef dict, CFStringRef key)
{
    auto *value = static_cast<CFStringRef>(CFDictionaryGetValue(dict, key));
    return value ? QString::fromCFString(value) : QString();
}
bool cfEnabled(CFDictionaryRef dict, CFStringRef key)
{
    auto *value = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, key));
    int enabled = 0;
    return value && CFNumberGetValue(value, kCFNumberIntType, &enabled) && enabled;
}
QNetworkProxy proxyFromDictionary(CFDictionaryRef dict)
{
    auto *type = static_cast<CFStringRef>(CFDictionaryGetValue(dict, kCFProxyTypeKey));
    if (!type) return QNetworkProxy(QNetworkProxy::DefaultProxy);
    if (type && CFStringCompare(type, kCFProxyTypeNone, 0) == kCFCompareEqualTo)
        return QNetworkProxy(QNetworkProxy::NoProxy);
    QNetworkProxy::ProxyType kind = QNetworkProxy::DefaultProxy;
    if (CFStringCompare(type, kCFProxyTypeHTTP, 0) == kCFCompareEqualTo ||
        CFStringCompare(type, kCFProxyTypeHTTPS, 0) == kCFCompareEqualTo)
        kind = QNetworkProxy::HttpProxy;
    else if (CFStringCompare(type, kCFProxyTypeSOCKS, 0) == kCFCompareEqualTo)
        kind = QNetworkProxy::Socks5Proxy;
    else if (CFStringCompare(type, kCFProxyTypeFTP, 0) == kCFCompareEqualTo)
        kind = QNetworkProxy::FtpCachingProxy;
    else
        return QNetworkProxy(QNetworkProxy::DefaultProxy);
    int port = 0;
    if (auto *number = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, kCFProxyPortNumberKey)))
        CFNumberGetValue(number, kCFNumberIntType, &port);
    return QNetworkProxy(kind, cfString(dict, kCFProxyHostNameKey), quint16(port));
}
QList<QNetworkProxy> manualProxies(CFDictionaryRef dict, const QString &scheme)
{
    QList<QNetworkProxy> proxies;
    const bool secure = scheme == QStringLiteral("https");
    CFStringRef enable = secure ? kCFNetworkProxiesHTTPSEnable : kCFNetworkProxiesHTTPEnable;
    CFStringRef host = secure ? kCFNetworkProxiesHTTPSProxy : kCFNetworkProxiesHTTPProxy;
    CFStringRef portKey = secure ? kCFNetworkProxiesHTTPSPort : kCFNetworkProxiesHTTPPort;
    if (cfEnabled(dict, enable)) {
        const QString name = cfString(dict, host);
        int port = 0;
        if (auto *number = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, portKey)))
            CFNumberGetValue(number, kCFNumberIntType, &port);
        if (!name.isEmpty() && port > 0 && port <= 65535) {
            proxies.append(QNetworkProxy(QNetworkProxy::HttpProxy, name, quint16(port)));
        }
    }
    if (cfEnabled(dict, kCFNetworkProxiesSOCKSEnable)) {
        const QString name = cfString(dict, kCFNetworkProxiesSOCKSProxy);
        int port = 0;
        if (auto *number = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, kCFNetworkProxiesSOCKSPort)))
            CFNumberGetValue(number, kCFNumberIntType, &port);
        if (!name.isEmpty() && port > 0 && port <= 65535) {
            proxies.append(QNetworkProxy(QNetworkProxy::Socks5Proxy, name, quint16(port)));
        }
    }
    return proxies;
}
struct PacContext {
    QList<QNetworkProxy> proxies;
    bool done = false;
    // False when the callback reported an error or returned no proxy list;
    // a default-constructed proxy must never be mistaken for "DIRECT".
    bool answered = false;
};
void pacCallback(void *client, CFArrayRef proxylist, CFErrorRef error)
{
    auto *context = static_cast<PacContext *>(client);
    context->done = true;
    if (!error && proxylist && CFArrayGetCount(proxylist) > 0) {
        for (CFIndex index = 0; index < CFArrayGetCount(proxylist); ++index) {
            const auto proxy = proxyFromDictionary(static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(proxylist, index)));
            if (proxy.type() != QNetworkProxy::DefaultProxy) context->proxies.append(proxy);
        }
        context->answered = !context->proxies.isEmpty();
    }
}
// Outcome of a PAC evaluation. A successful PAC that answers DIRECT must not be
// confused with a broken PAC, otherwise the manual proxy would override it.
enum class PacResult { Failed, Direct, Proxied };
PacResult runPac(const QString &location, const QUrl &url, QList<QNetworkProxy> &proxies)
{
    const QByteArray encodedUrl = url.toEncoded();
    if (encodedUrl.isEmpty()) return PacResult::Failed;
    const QByteArray locationUtf8 = location.toUtf8();
    auto cfLocationString = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(locationUtf8.constData()),
                                                    locationUtf8.size(), kCFStringEncodingUTF8, false);
    if (!cfLocationString) return PacResult::Failed;
    auto cfLocation = CFURLCreateWithString(nullptr, cfLocationString, nullptr);
    CFRelease(cfLocationString);
    if (!cfLocation) return PacResult::Failed;
    auto target = CFURLCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(encodedUrl.constData()),
                                       encodedUrl.size(), kCFStringEncodingUTF8, nullptr);
    if (!target) { CFRelease(cfLocation); return PacResult::Failed; }
    PacContext context;
    CFStreamClientContext clientContext{0, &context, nullptr, nullptr, nullptr};
    auto source = CFNetworkExecuteProxyAutoConfigurationURL(cfLocation, target, &pacCallback, &clientContext);
    CFRelease(cfLocation);
    CFRelease(target);
    if (!source) return PacResult::Failed;
    static CFStringRef mode = CFSTR("osmProxyResolve");
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, mode);
    const auto deadline = CFAbsoluteTimeGetCurrent() + 10.0;
    while (!context.done && CFAbsoluteTimeGetCurrent() < deadline &&
           !QThread::currentThread()->isInterruptionRequested())
        CFRunLoopRunInMode(mode, 0.05, true);
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, mode);
    CFRunLoopSourceInvalidate(source);
    CFRelease(source);
    if (!context.done || !context.answered) return PacResult::Failed;
    proxies = context.proxies;
    return proxies.first().type() == QNetworkProxy::NoProxy ? PacResult::Direct : PacResult::Proxied;
}
#endif
} // namespace

bool SystemProxy::hostExcluded(const QString &host, const QStringList &patterns,
                               bool excludeSimpleHostnames)
{
    return hostExcludedImpl(host, patterns, excludeSimpleHostnames);
}

QList<QNetworkProxy> SystemProxy::resolve(const QUrl &url, QString *failure)
{
    if (failure) failure->clear();
#ifdef Q_OS_MACOS
    CFDictionaryRef dict = CFNetworkCopySystemProxySettings();
    if (!dict) {
        if (failure) *failure = QObject::tr("无法读取系统代理配置");
        return {};
    }
    struct SettingsGuard {
        CFDictionaryRef value;
        ~SettingsGuard() { CFRelease(value); }
    } guard{dict};
    // Prefer an exact PAC result, but never let a broken PAC script mask the
    // manually configured proxy: Qt returns no proxy in that case, which
    // silently turns "system proxy" into a direct connection.
    const QString pac = cfString(dict, kCFNetworkProxiesProxyAutoConfigURLString).trimmed();
    const bool pacEnabled = cfEnabled(dict, kCFNetworkProxiesProxyAutoConfigEnable) && !pac.isEmpty();
    QStringList patterns;
    if (auto *exceptions = static_cast<CFArrayRef>(
            CFDictionaryGetValue(dict, kCFNetworkProxiesExceptionsList))) {
        for (CFIndex i = 0; i < CFArrayGetCount(exceptions); ++i) {
            if (auto *item = static_cast<CFStringRef>(CFArrayGetValueAtIndex(exceptions, i)))
                patterns.append(QString::fromCFString(item));
        }
    }
    const bool excludeSimple =
        cfEnabled(dict, kCFNetworkProxiesExcludeSimpleHostnames);
    if (hostExcludedImpl(url.host(), patterns, excludeSimple)) {
        return {QNetworkProxy(QNetworkProxy::NoProxy)};
    }
    const auto manual = manualProxies(dict, url.scheme());
    if (pacEnabled) {
        QList<QNetworkProxy> proxies;
        if (runPac(pac, url, proxies) != PacResult::Failed) return proxies;
        if (!manual.isEmpty()) return manual;
        if (failure) *failure = QObject::tr("系统 PAC 代理解析失败，请检查自动代理配置");
        return {};
    }
    if (!manual.isEmpty()) return manual;
    return {QNetworkProxy(QNetworkProxy::NoProxy)};
#else
    return QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
#endif
}

QByteArray CredentialStore::read(const QString &key, QString *error) {
    setError(error, {});
#ifdef Q_OS_MACOS
    auto query = credentialQuery(key);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status == errSecItemNotFound) return {};
    if (status != errSecSuccess) { setError(error, QStringLiteral("Keychain read failed (%1)").arg(status)); return {}; }
    const auto data = static_cast<CFDataRef>(result);
    const QByteArray bytes(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), int(CFDataGetLength(data)));
    CFRelease(result);
    return bytes;
#elif defined(Q_OS_WIN)
    const std::wstring target = (QString::fromLatin1(CredentialService) + QLatin1Char('/') + key).toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        if (GetLastError() != ERROR_NOT_FOUND) setError(error, QStringLiteral("Credential Manager read failed (%1)").arg(GetLastError()));
        return {};
    }
    const QByteArray bytes(reinterpret_cast<const char *>(credential->CredentialBlob), int(credential->CredentialBlobSize));
    CredFree(credential);
    return bytes;
#elif defined(CLIENT_QT_HAS_LIBSECRET)
    GError *failure = nullptr;
    const QByteArray utf8 = key.toUtf8();
    gchar *value = secret_password_lookup_sync(&CredentialSchema, nullptr, &failure, "key", utf8.constData(), nullptr);
    if (failure) { setError(error, QStringLiteral("Secret Service read failed")); g_error_free(failure); return {}; }
    const QByteArray result = value ? QByteArray::fromBase64(value) : QByteArray();
    if (value) secret_password_free(value);
    return result;
#else
    Q_UNUSED(key);
    setError(error, QStringLiteral("System credential storage is unavailable (install libsecret and rebuild)"));
    return {};
#endif
}

bool CredentialStore::write(const QString &key, const QByteArray &value, QString *error) {
    setError(error, {});
#ifdef Q_OS_MACOS
    auto query = credentialQuery(key);
    auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(value.constData()), value.size());
    auto updates = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(updates, kSecValueData, data);
    OSStatus status = SecItemUpdate(query, updates);
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, data);
        status = SecItemAdd(query, nullptr);
    }
    CFRelease(data); CFRelease(query); CFRelease(updates);
    if (status != errSecSuccess) setError(error, QStringLiteral("Keychain write failed (%1)").arg(status));
    return status == errSecSuccess;
#elif defined(Q_OS_WIN)
    if (value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) { setError(error, QStringLiteral("Credential exceeds Credential Manager limit")); return false; }
    std::wstring target = (QString::fromLatin1(CredentialService) + QLatin1Char('/') + key).toStdWString();
    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = &target[0];
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = DWORD(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(value.constData()));
    if (CredWriteW(&credential, 0)) return true;
    setError(error, QStringLiteral("Credential Manager write failed (%1)").arg(GetLastError()));
    return false;
#elif defined(CLIENT_QT_HAS_LIBSECRET)
    GError *failure = nullptr;
    const QByteArray utf8 = key.toUtf8(), encoded = value.toBase64();
    const bool ok = secret_password_store_sync(&CredentialSchema, SECRET_COLLECTION_DEFAULT,
        "JumpServer Client Qt", encoded.constData(), nullptr, &failure, "key", utf8.constData(), nullptr);
    if (failure) { setError(error, QStringLiteral("Secret Service write failed")); g_error_free(failure); }
    return ok;
#else
    Q_UNUSED(key); Q_UNUSED(value);
    setError(error, QStringLiteral("System credential storage is unavailable (install libsecret and rebuild)"));
    return false;
#endif
}

bool CredentialStore::remove(const QString &key, QString *error) {
    setError(error, {});
#ifdef Q_OS_MACOS
    auto query = credentialQuery(key);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    if (status == errSecSuccess || status == errSecItemNotFound) return true;
    setError(error, QStringLiteral("Keychain delete failed (%1)").arg(status));
    return false;
#elif defined(Q_OS_WIN)
    const std::wstring target = (QString::fromLatin1(CredentialService) + QLatin1Char('/') + key).toStdWString();
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) return true;
    setError(error, QStringLiteral("Credential Manager delete failed (%1)").arg(GetLastError()));
    return false;
#elif defined(CLIENT_QT_HAS_LIBSECRET)
    GError *failure = nullptr;
    const QByteArray utf8 = key.toUtf8();
    secret_password_clear_sync(&CredentialSchema, nullptr, &failure, "key", utf8.constData(), nullptr);
    if (failure) { setError(error, QStringLiteral("Secret Service delete failed")); g_error_free(failure); return false; }
    return true;
#else
    Q_UNUSED(key);
    setError(error, QStringLiteral("System credential storage is unavailable"));
    return false;
#endif
}

ConfigStore::ConfigStore(QObject *parent, const QString &path)
    : QObject(parent), path_(path.isEmpty() ? configPath() : path) {}
QString ConfigStore::platformKey() {
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}
QString ConfigStore::configPath() {
#ifdef Q_OS_MACOS
    const QString root = QDir::homePath() + QStringLiteral("/Library/Application Support");
#elif defined(Q_OS_WIN)
    const QString root = qEnvironmentVariable("APPDATA", QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
#else
    const QString root = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + QStringLiteral("/.config"));
#endif
    return QDir(root).filePath(QStringLiteral("jumpserver-client/config.json"));
}
QJsonObject ConfigStore::merge(const QJsonObject &user, const QJsonObject &defaults) {
    QJsonObject result = defaults;
    for (const QString &os : {QStringLiteral("windows"), QStringLiteral("macos"), QStringLiteral("linux")}) {
        QJsonObject mergedOS = defaults.value(os).toObject();
        const QJsonObject userOS = user.value(os).toObject();
        for (const QString &category : {QStringLiteral("terminal"), QStringLiteral("remotedesktop"), QStringLiteral("filetransfer"), QStringLiteral("databases")}) {
            QJsonArray items = mergedOS.value(category).toArray();
            for (int i = 0; i < items.size(); ++i) {
                QJsonObject item = items.at(i).toObject();
                for (const QJsonValue &existing : userOS.value(category).toArray()) {
                    const QJsonObject old = existing.toObject();
                    if (old.value("name") != item.value("name")) continue;
                    for (const QString &field : {QStringLiteral("match_first"), QStringLiteral("is_set")})
                        if (old.contains(field)) item.insert(field, old.value(field));
                    if (!old.value("path").toString().isEmpty()) item.insert("path", old.value("path"));
                    break;
                }
                items.replace(i, item);
            }
            mergedOS.insert(category, items);
        }
        result.insert(os, mergedOS);
    }
    return result;
}
bool ConfigStore::save(const QJsonObject &config) {
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) { error_ = tr("无法创建应用配置目录"); return false; }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) { error_ = file.errorString(); return false; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray bytes = QJsonDocument(config).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) { error_ = file.errorString(); return false; }
    return true;
}
bool ConfigStore::load(QJsonObject &config) {
    error_.clear();
    QFile resource(QStringLiteral(":/config.json"));
    if (!resource.open(QIODevice::ReadOnly)) { error_ = tr("缺少默认应用配置"); return false; }
    QJsonParseError parseError;
    const auto defaultsDoc = QJsonDocument::fromJson(resource.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !defaultsDoc.isObject()) { error_ = tr("默认应用配置格式错误"); return false; }
    const QJsonObject defaults = defaultsDoc.object();
    QFile file(path_);
    if (!file.exists()) { config = defaults; return save(config); }
    if (!file.open(QIODevice::ReadOnly)) { error_ = file.errorString(); return false; }
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject() || !doc.object().value(platformKey()).isObject()) {
        // A corrupt existing file must remain recoverable, never overwrite it silently.
        error_ = tr("现有应用配置损坏，请备份并修复：%1").arg(path_); return false;
    }
    config = doc.object();
    if (defaults.value("version").toInt(1) > config.value("version").toInt(1)) {
        config = merge(config, defaults); return save(config);
    }
    return true;
}
QJsonObject ConfigStore::applications() {
    QJsonObject config;
    if (!load(config)) return {};
    return config.value(platformKey()).toObject();
}
bool ConfigStore::selectApplication(const QString &category, const QString &protocol,
                                    const QString &name, const QString &path) {
    QJsonObject config;
    if (!load(config)) return false;
    QJsonObject os = config.value(platformKey()).toObject();
    QJsonArray items = os.value(category).toArray();
    int chosen = -1;
    for (int i = 0; i < items.size(); ++i) if (items.at(i).toObject().value("name").toString() == name) chosen = i;
    if (chosen < 0) { error_ = tr("未找到所选应用"); return false; }
    QJsonObject item = items.at(chosen).toObject();
    if (!path.trimmed().isEmpty()) {
        item.insert("path", path.trimmed()); item.insert("is_set", true); items.replace(chosen, item);
    } else {
        if (protocol.isEmpty() || !item.value("protocol").toArray().contains(protocol)) { error_ = tr("所选应用不支持该协议"); return false; }
        for (int i = 0; i < items.size(); ++i) {
            QJsonObject current = items.at(i).toObject();
            QJsonArray protocols;
            for (const QJsonValue &value : current.value("match_first").toArray()) if (value.toString() != protocol) protocols.append(value);
            if (i == chosen) protocols.append(protocol);
            current.insert("match_first", protocols); items.replace(i, current);
        }
    }
    os.insert(category, items); config.insert(platformKey(), os);
    if (!save(config)) return false;
    emit changed(); return true;
}
bool ConfigStore::clearDefaultApplication(const QString &category, const QString &protocol) {
    if (protocol.isEmpty()) { error_ = tr("未指定协议"); return false; }
    QJsonObject config;
    if (!load(config)) return false;
    QJsonObject os = config.value(platformKey()).toObject();
    QJsonArray items = os.value(category).toArray();
    bool cleared = false;
    for (int i = 0; i < items.size(); ++i) {
        QJsonObject item = items.at(i).toObject();
        const QJsonArray first = item.value("match_first").toArray();
        QJsonArray kept;
        for (const QJsonValue &value : first)
            if (value.toString() != protocol) kept.append(value);
        if (kept.size() == first.size()) continue;
        item.insert("match_first", kept);
        items.replace(i, item);
        cleared = true;
    }
    if (!cleared) return true;
    os.insert(category, items); config.insert(platformKey(), os);
    if (!save(config)) return false;
    emit changed(); return true;
}

Launcher::Launcher(QObject *parent, const QString &executable, const QString &config)
    : QObject(parent), executable_(executable), config_(config) {}
bool Launcher::isConnectionUrl(const QString &url) {
    if (!url.startsWith(QStringLiteral("jms://")) || url.size() > MaximumMessage) return false;
    const QByteArray encoded = url.mid(6).toLatin1();
    // fromBase64Encoding is Qt 5.15-only; fromBase64 exists on Qt 5.12 (Kylin V10 SP1).
    const QByteArray decoded = QByteArray::fromBase64(encoded);
    if (decoded.isEmpty()) return false;
    const auto doc = QJsonDocument::fromJson(decoded);
    if (!doc.isObject()) return false;
    const auto object = doc.object();
    return !object.value("protocol").toString().isEmpty() || !object.value("command").toString().isEmpty();
}
QString Launcher::executablePath() {
    // Resolve the packaged target architecture, including Rosetta and WOW64.
    QString arch = QSysInfo::buildCpuArchitecture();
#if defined(__loongarch64)
    // Older distribution Qt builds may report this architecture as "unknown".
    arch = QStringLiteral("loong64");
#elif defined(__riscv) && __riscv_xlen == 64
    arch = QStringLiteral("riscv64");
#endif
    if (arch == QStringLiteral("x86_64")) arch = QStringLiteral("amd64");
    else if (arch == QStringLiteral("i386")) arch = QStringLiteral("386");
    else if (arch == QStringLiteral("loongarch64")) arch = QStringLiteral("loong64");
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    else if (arch == QStringLiteral("mips64")) arch = QStringLiteral("mips64le");
#endif
    QString folder, executable;
#ifdef Q_OS_WIN
    folder = QStringLiteral("windows"); executable = arch == QStringLiteral("386") ? QStringLiteral("JumpServerClient32.exe") : QStringLiteral("JumpServerClient.exe");
#elif defined(Q_OS_MACOS)
    folder = QStringLiteral("darwin-") + arch; executable = QStringLiteral("JumpServerClient");
#else
    folder = QStringLiteral("linux-") + arch; executable = QStringLiteral("JumpServerClient");
#endif
    const QString dir = QCoreApplication::applicationDirPath();
    QStringList roots{dir + "/bin", dir + "/resources/bin", dir + "/../Resources/bin", dir + "/../Resources/resources/bin", dir + "/../lib/client/bin"};
#ifdef CLIENT_QT_RESOURCE_DIR
    roots.append(QString::fromUtf8(CLIENT_QT_RESOURCE_DIR) + "/bin");
#endif
    for (const QString &root : roots) {
        const QString candidate = QDir(root).filePath(folder + QLatin1Char('/') + executable);
        if (QFileInfo(candidate).isExecutable()) return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}
void Launcher::launch(const QString &url) {
    if (!isConnectionUrl(url)) { emit failed(tr("无效的客户端连接地址")); return; }
    ConfigStore config(nullptr, config_);
    config.applications();
    if (!config.error().isEmpty()) { emit failed(config.error()); return; }
    const QString executable = executable_.isEmpty() ? executablePath() : executable_;
    if (executable.isEmpty()) { emit failed(tr("未找到当前平台的协议启动器，请先构建或安装完整资源包")); return; }
    auto process = new QProcess(this);
    process->setProgram(executable);
    process->setArguments({url});
    process->setWorkingDirectory(QFileInfo(executable).absolutePath());
    process->setProcessChannelMode(QProcess::SeparateChannels);
    auto reported = std::make_shared<bool>(false);
    auto buffer = std::make_shared<QByteArray>();
    auto drain = [this, process, reported, buffer]() {
        buffer->append(process->readAllStandardError());
        process->readAllStandardOutput();
        const QByteArray lower = buffer->toLower();
        if (!*reported && (lower.contains("error:") || lower.contains("failed") || lower.contains("not configured"))) {
            *reported = true;
            emit failed(tr("协议启动器报告错误，请检查对应客户端的安装路径与配置"));
        }
        if (buffer->size() > 4096) *buffer = buffer->right(4096);
    };
    connect(process, &QProcess::readyReadStandardError, this, drain);
    connect(process, &QProcess::readyReadStandardOutput, this, [process]() { process->readAllStandardOutput(); });
    connect(process, &QProcess::started, this, &Launcher::started);
    connect(process, &QProcess::errorOccurred, this, [this, process, reported](QProcess::ProcessError failure) {
        if (!*reported) { *reported = true; emit failed(tr("无法运行协议启动器")); }
        if (failure == QProcess::FailedToStart) { emit finished(); process->deleteLater(); }
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process, reported, drain](int code, QProcess::ExitStatus exitStatus) {
        drain();
        if (!*reported && (code != 0 || exitStatus == QProcess::CrashExit)) emit failed(tr("协议启动器异常退出（%1）").arg(code));
        emit finished(); process->deleteLater();
    });
    process->start();
}

SingleInstance::SingleInstance(QObject *parent, const QString &name) : QObject(parent) {
    const QByteArray identity = QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    name_ = name.isEmpty() ? QStringLiteral("client-") + QString::fromLatin1(identity) : name;
}
SingleInstance::~SingleInstance() {
    if (server_) server_->close();
    lock_.reset();
}
bool SingleInstance::start(const QStringList &args) {
    error_.clear();
    if (server_) return true;
    const QByteArray payload = QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact) + '\n';
    if (payload.size() > MaximumMessage) { error_ = tr("启动参数过长"); return false; }
    auto forward = [this, &payload]() {
        QLocalSocket socket;
        socket.connectToServer(name_);
        if (!socket.waitForConnected(750)) return false;
        socket.write(payload);
        if (!socket.waitForBytesWritten(1000) && socket.bytesToWrite() != 0) return false;
        socket.disconnectFromServer();
        return true;
    };
    if (forward()) return false;
    const QString lockPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(name_ + ".lock");
    lock_.reset(new QLockFile(lockPath));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(1500)) {
        if (forward()) return false;
        error_ = tr("另一客户端实例正在启动或未响应"); return false;
    }
    // Only the lock holder may remove a stale server name.
    QLocalServer::removeServer(name_);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this, [this]() {
        while (server_->hasPendingConnections()) {
            auto socket = server_->nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            socket->setReadBufferSize(MaximumMessage + 1);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(3000, socket, [socket]() { socket->abort(); socket->deleteLater(); });
            connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer]() {
                buffer->append(socket->readAll());
                if (buffer->size() > MaximumMessage) { socket->abort(); return; }
                const int end = buffer->indexOf('\n');
                if (end < 0) return;
                const auto doc = QJsonDocument::fromJson(buffer->left(end));
                if (doc.isArray()) {
                    QStringList arguments;
                    for (const QJsonValue &value : doc.array()) if (value.isString()) arguments.append(value.toString());
                    emit message(arguments);
                }
                socket->disconnectFromServer();
            });
        }
    });
    if (!server_->listen(name_)) { error_ = server_->errorString(); delete server_; server_ = nullptr; lock_.reset(); return false; }
    return true;
}
