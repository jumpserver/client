#include "appservice.h"
#include "platform.h"

#include <QNetworkProxy>
#include <QSettings>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTcpSocket>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {
const QString oauthRoot = QStringLiteral("/core/auth/oauth2-provider/");
const QString assetRoot = QStringLiteral("/api/v1/perms/users/self/assets/");
const QString methodsSuperseded = QStringLiteral("__client_methods_superseded__");
const QString versionPath = QStringLiteral("/api/v1/settings/client/versions/");
const QString connectionTokenPath = QStringLiteral("/api/v1/authentication/connection-token/");
const QString ticketRoot = QStringLiteral("/api/v1/tickets/tickets/");
constexpr int ticketPollIntervalMs = 3000;

QString majorMinorVersion(const QString &version)
{
    const auto parts = version.trimmed().split('.');
    const QRegularExpression nonDigit(QStringLiteral("[^0-9]"));
    const auto major = parts.value(0).remove(nonDigit);
    const auto minor = parts.value(1).remove(nonDigit);
    return major.isEmpty() ? QString() : minor.isEmpty() ? major : major + '.' + minor;
}

bool compatibleVersions(const QJsonArray &values)
{
    QStringList versions, normalized;
    for (const auto &value : values) {
        if (value.isNull()) continue;
        const auto version = value.toVariant().toString();
        if (version.isEmpty()) continue;
        versions.append(version);
        const auto majorMinor = majorMinorVersion(version);
        if (!majorMinor.isEmpty()) normalized.append(majorMinor);
    }
    if (versions.isEmpty()) return true;
    const auto current = QString::fromLatin1(CLIENT_QT_VERSION);
    const auto currentMajorMinor = majorMinorVersion(current);
    if (!currentMajorMinor.isEmpty() && !normalized.isEmpty()) return normalized.contains(currentMajorMinor);
    return versions.contains(current);
}

QString randomSecret()
{
    QByteArray bytes(32, Qt::Uninitialized);
    auto *generator = QRandomGenerator::system();
    for (int i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(generator->generate() & 0xff);
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool secureEqual(const QString &a, const QString &b)
{
    if (a.size() != b.size() || a.isEmpty()) return false;
    ushort difference = 0;
    for (int i = 0; i < a.size(); ++i) difference |= a.at(i).unicode() ^ b.at(i).unicode();
    return difference == 0;
}

QByteArray form(const QUrlQuery &query) { return query.query(QUrl::FullyEncoded).toUtf8(); }

void addQueryItem(QUrlQuery &query, const QString &key, const QString &value)
{
    // QUrlQuery expects encoded input and deliberately leaves literal '+' unchanged.
    // Encode raw values first so HTML-form parsers preserve '+' and percent sequences.
    query.addQueryItem(QString::fromLatin1(QUrl::toPercentEncoding(key)),
                       QString::fromLatin1(QUrl::toPercentEncoding(value)));
}

QString callbackValue(const QUrlQuery &query, const QString &key)
{
    auto encoded = query.queryItemValue(key, QUrl::FullyEncoded).toUtf8();
    encoded.replace('+', ' ');
    return QUrl::fromPercentEncoding(encoded);
}

QUrlQuery callbackQuery(const QUrl &url)
{
    // Older redirect-confirm pages HTML-escape '&' inside a JavaScript URL.
    // Repair parameter names only; never decode or rewrite code/state values.
    const QSet<QString> fields{"state", "code", "error", "error_description"};
    QSet<QString> seen;
    QUrlQuery result;
    for (const auto &item : QUrlQuery(url).queryItems(QUrl::FullyEncoded)) {
        QString key = item.first;
        if (key.startsWith("amp;") && fields.contains(key.mid(4))) key = key.mid(4);
        if (fields.contains(key)) {
            if (seen.contains(key)) return {}; // Reject ambiguous callbacks.
            seen.insert(key);
        }
        result.addQueryItem(key, item.second);
    }
    return result;
}

QString pathId(const QString &id) { return QString::fromLatin1(QUrl::toPercentEncoding(id)); }

bool pageData(const QJsonValue &value, QJsonArray &items, int &count)
{
    if (value.isArray()) { items = value.toArray(); count = items.size(); return true; }
    const auto object = value.toObject();
    if (!object.value("results").isArray() || !object.value("count").isDouble()) return false;
    items = object.value("results").toArray(); count = object.value("count").toInt(-1);
    return count >= 0;
}

QString connectionFailureReason(const QByteArray &bytes)
{
    // Only emit local descriptions for recognized errors/field names. Never
    // display arbitrary response text, echoed input secrets, tokens or HTML.
    const auto object = QJsonDocument::fromJson(bytes).object();
    for (const auto &key : {QStringLiteral("error"), QStringLiteral("detail")}) {
        const QString value = object.value(key).toString();
        if (value.startsWith(QStringLiteral("Connect method not support:")))
            return AppService::tr("站点不支持所选连接方式，请在连接设置中选择站点提供的远程应用");
        if (value == QStringLiteral("No host account available, please check the applet, host and account"))
            return AppService::tr("远程应用没有可用的发布机账号，请检查应用发布状态和账号分配");
    }
    const std::vector<std::pair<QString, QString>> fields{
        {"asset", AppService::tr("资产")}, {"account", AppService::tr("授权账号")},
        {"protocol", AppService::tr("协议")}, {"connect_method", AppService::tr("连接方式")},
        {"connect_options", AppService::tr("连接选项")},
        {"input_username", AppService::tr("用户名")}, {"input_secret", AppService::tr("输入凭据")}};
    QStringList invalid;
    for (const auto &field : fields)
        if (object.contains(field.first)) invalid.append(field.second);
    return invalid.isEmpty() ? QString() : AppService::tr("站点拒绝以下连接参数：%1").arg(invalid.join("、"));
}

bool sameSiteUrl(const QUrl &url, const QUrl &site)
{
    const int defaultPort = site.scheme() == "https" ? 443 : 80;
    return url.isValid() && url.scheme() == site.scheme() && url.host() == site.host()
        && url.port(defaultPort) == site.port(defaultPort) && !url.authority(QUrl::FullyEncoded).contains('@');
}

QString ticketApiPath(const QJsonObject &endpoint, const QByteArray &method, const QString &path, const QUrl &site)
{
    if (endpoint.value("method").toString() != QString::fromLatin1(method)) return {};
    const QUrl url(endpoint.value("url").toString(), QUrl::StrictMode);
    if (!url.isValid() || url.hasQuery() || url.hasFragment()) return {};
    const QString encodedPath = url.path(QUrl::FullyEncoded);
    const QString mountedPath = site.path(QUrl::FullyEncoded) + path;
    if (url.isRelative() && url.authority().isEmpty() && (encodedPath == path || encodedPath == mountedPath))
        return path;
    if (sameSiteUrl(url, site) && encodedPath == mountedPath) return path;
    return {};
}
}

AppService::AppService(QObject *parent) : QObject(parent), m_callback(this)
{
    m_ticketTimer.setSingleShot(true);
    connect(&m_ticketTimer, &QTimer::timeout, this, &AppService::pollConnectionApproval);
    m_worker = new NetworkWorker;
    m_worker->moveToThread(&m_networkThread);
    connect(&m_networkThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_networkThread.start();
    m_loginTimer.setSingleShot(true);
    m_loginTimer.setInterval(5 * 60 * 1000);
    connect(&m_loginTimer, &QTimer::timeout, this, [this] { failLogin(tr("登录超时，请重试")); });
    applyProxy();
    m_sessionTimer.setSingleShot(true);
    connect(&m_sessionTimer, &QTimer::timeout, this, [this] {
        if (!checkSessionExpiry()) scheduleSessionExpiry();
    });
    connect(&m_callback, &QTcpServer::newConnection, this, [this] {
        while (m_callback.hasPendingConnections()) {
            auto *socket = m_callback.nextPendingConnection();
            socket->setParent(this);
            socket->setReadBufferSize(16384);
            const auto generation = m_generation;
            auto buffer = std::make_shared<QByteArray>();
            auto *timeout = new QTimer(socket);
            timeout->setSingleShot(true); timeout->start(5000);
            connect(timeout, &QTimer::timeout, socket, &QTcpSocket::abort);
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, generation] {
                buffer->append(socket->readAll());
                if (buffer->size() > 8192) { socket->abort(); return; }
                if (!buffer->contains("\r\n\r\n")) return;
                const auto firstLine = buffer->left(buffer->indexOf("\r\n")).split(' ');
                QUrl callback;
                bool valid = firstLine.size() == 3 && firstLine[0] == "GET" && generation == m_generation;
                if (valid) {
                    callback = QUrl(QStringLiteral("http://127.0.0.1:14876") + QString::fromUtf8(firstLine[1]));
                    valid = callback.path() == "/auth/callback" && !m_state.isEmpty()
                        && secureEqual(callbackValue(callbackQuery(callback), "state"), m_state);
                }
                const QByteArray body = valid ? QByteArray("Authorization received. Return to the OSM client.")
                                              : QByteArray("Invalid authorization callback.");
                socket->write(QByteArray("HTTP/1.1 ") + (valid ? "200 OK" : "400 Bad Request")
                    + "\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                    + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
                if (valid) acceptCallback(callback);
            });
        }
    });
}

QString AppService::credentialKey() const
{
    return QStringLiteral("oauth-") + QString::fromLatin1(QCryptographicHash::hash(m_site.toUtf8(), QCryptographicHash::Sha256).toHex());
}
AppService::~AppService()
{
    m_networkThread.requestInterruption();
    m_networkThread.quit();
    m_networkThread.wait();
}
void AppService::applyProxy() {
    const QSettings settings;
    QString mode = settings.value("proxy/mode", "system").toString().trimmed();
    if (mode != "manual" && mode != "none") mode = QStringLiteral("system");
    m_proxyMode = mode;
    m_proxyAddress = settings.value("proxy/http").toString().trimmed();
}

bool AppService::configureSite(QString site)
{
    site = site.trimmed();
    while (site.endsWith('/')) site.chop(1);
    const QUrl url(site);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http")
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
        emit error(tr("请输入有效的 HTTP 或 HTTPS 站点地址")); return false;
    }
    m_site = url.toString(QUrl::FullyEncoded);
    return true;
}

void AppService::clearPendingLogin()
{
    m_loginTimer.stop(); m_callback.close();
    m_state.clear(); m_verifier.clear(); m_redirect.clear();
}

void AppService::setLoginPending(bool pending)
{
    if (m_loggingIn == pending) return;
    m_loggingIn = pending;
    emit loginStateChanged(pending);
}

void AppService::resetSession()
{
    clearConnection();
    ++m_generation; ++m_assetRequest; ++m_orgGeneration;
    const auto generation = m_generation;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, generation] {
        worker->cancelBefore(generation);
    }, Qt::QueuedConnection);
    clearPendingLogin();
    m_sessionTimer.stop();
    m_access.clear(); m_refresh.clear(); m_clientId.clear(); m_org.clear(); m_expires = 0;
    m_refreshLifetime = 0; m_refreshExpiresAt = 0;
    m_refreshing = false; m_refreshWaiters.clear(); setLoginPending(false);
    emit contextChanged();
}

bool AppService::checkSessionExpiry()
{
    if (m_access.isEmpty() || m_refreshExpiresAt <= 0 ||
        QDateTime::currentSecsSinceEpoch() < m_refreshExpiresAt) return false;
    endSession(tr("登录已过期，请重新登录"));
    return true;
}

void AppService::scheduleSessionExpiry()
{
    m_sessionTimer.stop();
    if (m_access.isEmpty() || m_refreshExpiresAt <= 0) return;
    const qint64 remaining = m_refreshExpiresAt - QDateTime::currentSecsSinceEpoch();
    // Periodic checks handle clock adjustments and avoid QTimer's integer limit.
    m_sessionTimer.start(static_cast<int>(qBound(qint64(1), remaining, qint64(60)) * 1000));
}

void AppService::login(QString site, bool loopback)
{
    // Switching identities invalidates in-flight replies before contacting the next site.
    const QString oldSite = m_site;
    if (!configureSite(site)) { m_site = oldSite; return; }
    applyProxy();
    resetSession(); emit loggedOut(); setLoginPending(true);
    m_loginTimer.start(); emit status(tr("正在检查站点版本…"), true);
    request("GET", versionPath, {}, {}, {}, false,
        [this, loopback](QJsonValue value, QString failure, int code) {
            emit metadataReady("version", failure.isEmpty() ? value : QJsonValue(QJsonValue::Null));
            if (code == 404) { failLogin(tr("站点不支持当前客户端版本，请升级站点或使用兼容客户端"), true); return; }
            if (!failure.isEmpty()) { failLogin(tr("无法检查站点客户端版本：%1").arg(failure), true); return; }
            if (!value.isArray()) { failLogin(tr("站点返回了无效的客户端版本列表"), true); return; }
            if (!compatibleVersions(value.toArray())) {
                failLogin(tr("当前客户端版本 %1 与站点支持的主次版本不匹配，请使用兼容版本").arg(QString::fromLatin1(CLIENT_QT_VERSION)), true);
                return;
            }
            beginAuthorization(loopback);
        });
}

void AppService::beginAuthorization(bool loopback)
{
    emit status(tr("正在获取站点认证配置…"));
    request("GET", oauthRoot + ".well-known/oauth-authorization-server", {}, {}, {}, false,
        [this, loopback](QJsonValue value, QString failure, int) {
        if (!failure.isEmpty()) { failLogin(failure); return; }
        m_clientId = value.toObject().value("client_id").toString();
        if (m_clientId.isEmpty()) { failLogin(tr("站点 OAuth 配置缺少 client_id")); return; }
        const auto lifetime = value.toObject().value("refresh_token_expires_in");
        if (!lifetime.isUndefined() && !lifetime.isNull()) {
            const double seconds = lifetime.toDouble(-1);
            if (!lifetime.isDouble() || !std::isfinite(seconds) || seconds < 0 ||
                std::floor(seconds) != seconds || seconds > 9007199254740991.0) {
                failLogin(tr("站点返回了无效的刷新令牌有效期")); return;
            }
            m_refreshLifetime = static_cast<qint64>(seconds);
        }
        m_redirect = loopback ? QStringLiteral("http://127.0.0.1:14876/auth/callback") : QStringLiteral("jms://auth/callback");
        if (loopback && !m_callback.listen(QHostAddress::LocalHost, 14876)) {
            failLogin(tr("无法监听登录回调端口 14876，请关闭占用端口的客户端")); return;
        }
        m_state = randomSecret(); m_verifier = randomSecret();
        const auto challenge = QCryptographicHash::hash(m_verifier.toUtf8(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        QUrl url(m_site + oauthRoot + "authorize/");
        QUrlQuery query;
        addQueryItem(query, "client_id", m_clientId); addQueryItem(query, "response_type", "code");
        addQueryItem(query, "redirect_uri", m_redirect); addQueryItem(query, "scope", "write read");
        addQueryItem(query, "state", m_state); addQueryItem(query, "code_challenge", QString::fromLatin1(challenge));
        addQueryItem(query, "code_challenge_method", "S256"); url.setQuery(query);
        emit status(tr("请在浏览器完成登录")); emit authorizationUrlReady(url);
        if (!QDesktopServices::openUrl(url)) failLogin(tr("无法打开系统浏览器"));
    });
}

void AppService::cancelLogin()
{
    if (!m_loggingIn) return;
    resetSession(); emit status(tr("已取消登录")); emit loggedOut();
}

void AppService::failLogin(const QString &message, bool statusOnly)
{
    resetSession(); emit error(message, statusOnly); emit loggedOut();
}

void AppService::acceptCallback(QUrl url)
{
    if (!m_loggingIn || m_state.isEmpty()) return;
    const bool deepLink = url.scheme() == "jms" && url.host() == "auth" && url.path() == "/callback";
    const bool loopback = url.scheme() == "http" && url.host() == "127.0.0.1" && url.port() == 14876 && url.path() == "/auth/callback";
    if ((m_redirect.startsWith("jms:") && !deepLink) || (m_redirect.startsWith("http:") && !loopback)) return;
    const QUrlQuery query = callbackQuery(url);
    if (!secureEqual(callbackValue(query, "state"), m_state)) {
        emit error(tr("登录回调 state 无效，已拒绝该回调")); return;
    }
    if (query.hasQueryItem("error")) { failLogin(tr("站点拒绝或取消了授权")); return; }
    const auto code = callbackValue(query, "code");
    if (code.isEmpty()) { failLogin(tr("登录回调缺少授权码")); return; }
    QUrlQuery body;
    addQueryItem(body, "grant_type", "authorization_code"); addQueryItem(body, "client_id", m_clientId);
    addQueryItem(body, "code", code); addQueryItem(body, "code_verifier", m_verifier);
    addQueryItem(body, "redirect_uri", m_redirect);
    clearPendingLogin(); m_loginTimer.start();
    emit status(tr("正在验证登录…"));
    request("POST", oauthRoot + "token/", {}, form(body), "application/x-www-form-urlencoded", false,
        [this](QJsonValue value, QString failure, int) {
            if (!failure.isEmpty()) { failLogin(failure); return; }
            if (!applyToken(value.toObject())) { failLogin(tr("站点未返回有效 Bearer 令牌")); return; }
            m_refreshExpiresAt = m_refreshLifetime > 0 ? QDateTime::currentSecsSinceEpoch() + m_refreshLifetime : 0;
            scheduleSessionExpiry();
            loadIdentity();
        });
}

bool AppService::applyToken(const QJsonObject &token)
{
    const auto access = token.value("access_token").toString();
    const auto type = token.value("token_type").toString("Bearer");
    if (access.isEmpty() || type.compare("Bearer", Qt::CaseInsensitive) != 0) return false;
    m_access = access;
    if (!token.value("refresh_token").toString().isEmpty()) m_refresh = token.value("refresh_token").toString();
    const qint64 expiresIn = static_cast<qint64>(token.value("expires_in").toDouble());
    m_expires = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
    return true;
}

void AppService::persist()
{
    if (!m_persistCredentials) return;
    QJsonObject value{{"access_token", m_access}, {"refresh_token", m_refresh}, {"client_id", m_clientId},
                      {"expires_at", static_cast<double>(m_expires)},
                      {"refresh_expires_at", static_cast<double>(m_refreshExpiresAt)},
                      {"refresh_expiry_known", true}};
    QString failure;
    if (!CredentialStore::write(credentialKey(), QJsonDocument(value).toJson(QJsonDocument::Compact), &failure))
        emit status(tr("已登录，但系统凭据库不可用；本次登录仅在内存中保留"));
}

void AppService::restore(QString site)
{
    const QString oldSite = m_site;
    if (!configureSite(site)) { m_site = oldSite; return; }
    applyProxy();
    resetSession(); emit loggedOut();
    setLoginPending(true); m_loginTimer.start();
    QString failure;
    const auto payload = m_persistCredentials ? CredentialStore::read(credentialKey(), &failure) : QByteArray();
    const auto record = QJsonDocument::fromJson(payload).object();
    m_access = record.value("access_token").toString(); m_refresh = record.value("refresh_token").toString();
    m_clientId = record.value("client_id").toString(); m_expires = static_cast<qint64>(record.value("expires_at").toDouble());
    if (m_access.isEmpty()) {
        clearPendingLogin(); setLoginPending(false);
        emit status(tr("未找到可恢复的登录，请使用浏览器登录")); return;
    }
    if (!record.value("refresh_expiry_known").toBool()) {
        QString ignored;
        if (m_persistCredentials) CredentialStore::remove(credentialKey(), &ignored);
        resetSession(); emit loggedOut(); emit status(tr("登录已过期，请重新登录")); return;
    }
    const auto deadline = record.value("refresh_expires_at");
    if (!deadline.isDouble() || deadline.toDouble() < 0 || deadline.toDouble() > 9007199254740991.0 ||
        std::floor(deadline.toDouble()) != deadline.toDouble()) {
        QString ignored;
        if (m_persistCredentials) CredentialStore::remove(credentialKey(), &ignored);
        resetSession(); emit loggedOut(); emit status(tr("登录已过期，请重新登录")); return;
    }
    m_refreshExpiresAt = static_cast<qint64>(deadline.toDouble());
    if (checkSessionExpiry()) return;
    scheduleSessionExpiry();
    ensureToken([this](bool ok) { if (ok) loadIdentity(); else failLogin(tr("登录已失效，请重新登录")); });
}

void AppService::ensureToken(Ready ready, bool force)
{
    if (checkSessionExpiry()) return;
    if (m_access.isEmpty()) { ready(false); return; }
    const bool expired = m_expires > 0 && m_expires <= QDateTime::currentSecsSinceEpoch() + 60;
    if (!force && !expired) { ready(true); return; }
    if (m_refresh.isEmpty() || m_clientId.isEmpty()) { ready(false); return; }
    m_refreshWaiters.push_back(std::move(ready));
    if (m_refreshing) return;
    m_refreshing = true;
    QUrlQuery body;
    addQueryItem(body, "grant_type", "refresh_token"); addQueryItem(body, "client_id", m_clientId);
    addQueryItem(body, "refresh_token", m_refresh);
    request("POST", oauthRoot + "token/", {}, form(body), "application/x-www-form-urlencoded", false,
        [this](QJsonValue value, QString failure, int) {
            const bool ok = failure.isEmpty() && applyToken(value.toObject());
            m_refreshing = false;
            auto waiting = std::move(m_refreshWaiters); m_refreshWaiters.clear();
            if (ok) persist();
            const auto generation = m_generation;
            for (const auto &callback : waiting) { if (generation != m_generation) break; callback(ok); }
        });
}

void AppService::loadIdentity()
{
    struct Identity { QJsonObject profile, org, settings; QJsonValue permissions; int remaining = 4; bool failed = false; };
    auto identity = std::make_shared<Identity>();
    auto finish = [this, identity] {
        if (--identity->remaining != 0 || identity->failed) return;
        if (identity->profile.value("id").toString().isEmpty()) { failLogin(tr("站点返回了无效用户信息")); return; }
        clearPendingLogin(); setLoginPending(false);
        m_org = identity->org.value("id").toString();
        emit status(tr("登录成功")); persist();
        emit loggedIn(identity->profile, identity->permissions, identity->org, identity->settings);
        metadata();
    };
    request("GET", "/api/v1/users/profile/", {}, {}, {}, true, [this, identity, finish](QJsonValue v, QString e, int) {
        if (!e.isEmpty()) { identity->failed = true; failLogin(e); return; }
        identity->profile = v.toObject(); finish();
    });
    request("GET", "/api/v1/users/profile/permissions/", {}, {}, {}, true, [this, identity, finish](QJsonValue v, QString e, int) {
        if (!e.isEmpty()) { identity->failed = true; failLogin(e); return; }
        identity->permissions = v; finish();
    });
    request("GET", "/api/v1/orgs/orgs/current/", {}, {}, {}, true, [this, identity, finish](QJsonValue v, QString e, int) {
        if (!e.isEmpty()) { identity->failed = true; failLogin(e); return; }
        identity->org = v.toObject(); finish();
    });
    request("GET", "/api/v1/settings/public/", {}, {}, {}, true, [identity, finish](QJsonValue v, QString e, int) {
        identity->settings = e.isEmpty() ? v.toObject() : QJsonObject{{"SECURITY_LUNA_REMEMBER_AUTH", false}};
        finish();
    });
}

void AppService::logout()
{
    endSession(tr("已退出登录"));
}

void AppService::endSession(const QString &message)
{
    QUrlQuery body;
    addQueryItem(body, "client_id", m_clientId);
    addQueryItem(body, "token", m_refresh.isEmpty() ? m_access : m_refresh);
    addQueryItem(body, "token_type_hint", m_refresh.isEmpty() ? "access_token" : "refresh_token");
    const bool revoke = !m_access.isEmpty() || !m_refresh.isEmpty();
    QNetworkRequest revokeRequest(QUrl(m_site + oauthRoot + "revoke/"));
    revokeRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    revokeRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    const auto revokeBody = form(body);
    const auto mode = m_proxyMode;
    const auto address = m_proxyAddress;
    QString failure;
    if (m_persistCredentials) CredentialStore::remove(credentialKey(), &failure);
    resetSession();
    if (revoke) {
        auto *worker = m_worker;
        const auto generation = m_generation;
        QMetaObject::invokeMethod(worker, [worker, revokeRequest, revokeBody, mode, address, generation] {
            worker->send(revokeRequest, "POST", revokeBody, mode, address, generation,
                [](NetworkResult result) {
                    if (!result.failure.isEmpty() || result.error != QNetworkReply::NoError || result.status < 200 || result.status >= 300)
                        qWarning("OAuth token revocation failed (HTTP %d, network error %d)", result.status, int(result.error));
                }, true);
        }, Qt::QueuedConnection);
    }
    emit loggedOut(); emit status(message);
}

void AppService::setOrg(QString org)
{
    if (m_org == org) return;
    clearConnection();
    m_org = org; ++m_orgGeneration; ++m_assetRequest;
    emit contextChanged();
}

void AppService::request(const QByteArray &method, const QString &path, const QUrlQuery &query,
                         const QByteArray &body, const QByteArray &contentType, bool auth, Result result)
{
    if (auth && checkSessionExpiry()) return;
    QUrl url(m_site + path); url.setQuery(query);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "OSMClient/" CLIENT_QT_VERSION);
    const int offset = QDateTime::currentDateTime().offsetFromUtc();
    const auto timezone = QStringLiteral("%1%2:%3").arg(offset < 0 ? '-' : '+')
        .arg(qAbs(offset) / 3600, 2, 10, QLatin1Char('0')).arg(qAbs(offset) % 3600 / 60, 2, 10, QLatin1Char('0'));
    request.setRawHeader("X-TZ", timezone.toLatin1());
    request.setRawHeader("Referer", url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toEncoded());
    if (!contentType.isEmpty()) request.setRawHeader("Content-Type", contentType);
    if (auth) request.setRawHeader("Authorization", "Bearer " + m_access.toUtf8());
    if (auth && !m_org.isEmpty()) request.setRawHeader("X-JMS-ORG", m_org.toUtf8());
    const auto generation = m_generation;
    const auto completion = [this, generation, result, method, path](NetworkResult response) {
        if (generation != m_generation) return;
        if (checkSessionExpiry()) return;
        const int code = response.status;
        if (!response.failure.isEmpty() || response.error != QNetworkReply::NoError || code < 200 || code >= 300) {
            // Do not surface arbitrary response bodies: they can contain credentials.
            QString failure;
            if (!response.failure.isEmpty()) failure = response.failure;
            else if (code == 401) failure = tr("登录已过期，请重新登录（HTTP 401）");
            else if (code == 403) failure = tr("操作被拒绝，请检查授权、访问控制或审批要求（HTTP 403）");
            else if (code > 0) failure = tr("站点请求失败（HTTP %1）").arg(code);
            else if (response.error == QNetworkReply::SslHandshakeFailedError)
                failure = tr("TLS 连接失败，请检查站点的 HTTPS 服务与协议配置");
            else if (response.error >= QNetworkReply::ProxyConnectionRefusedError && response.error <= QNetworkReply::UnknownProxyError)
                failure = tr("代理连接失败，请检查代理服务与认证配置");
            else if (response.error == QNetworkReply::HostNotFoundError)
                failure = tr("站点域名解析失败，请检查 DNS 与网络配置");
            else failure = tr("网络请求失败或超时，请检查网络与站点地址");
            if (code == 400 && response.failure.isEmpty() && path.startsWith(connectionTokenPath)) {
                const auto bytes = response.body.left(65537);
                if (bytes.size() <= 65536) {
                    if (method == "POST" && path == connectionTokenPath) {
                        const auto businessCode = QJsonDocument::fromJson(bytes).object().value("code").toString();
                        if (businessCode == "acl_review" || businessCode == "acl_reject") {
                            result(QJsonObject{{"code", businessCode}}, failure, code);
                            return;
                        }
                    }
                    const auto reason = connectionFailureReason(bytes);
                    if (!reason.isEmpty()) failure = reason + tr("（HTTP 400）");
                }
            }
            result({}, failure, code); return;
        }
        const auto bytes = response.body;
        if (bytes.trimmed().isEmpty()) { result({}, {}, code); return; }
        if (method == "PUT" && path.startsWith(ticketRoot) && path.endsWith("/close/")) {
            const auto acknowledgement = bytes.trimmed();
            if (acknowledgement == "\"ok\"" || acknowledgement == "ok") {
                result({}, {}, code); return;
            }
        }
        QJsonParseError parseError;
        const auto json = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError) { result({}, tr("站点返回了无效 JSON"), code); return; }
        result(json.isArray() ? QJsonValue(json.array()) : QJsonValue(json.object()), {}, code);
    };
    auto *worker = m_worker;
    const auto mode = m_proxyMode;
    const auto address = m_proxyAddress;
    QMetaObject::invokeMethod(worker, [this, worker, request, method, body, mode, address, generation, completion] {
        worker->send(request, method, body, mode, address, generation,
                     [this, completion](NetworkResult response) {
            QMetaObject::invokeMethod(this, [completion, response] { completion(response); }, Qt::QueuedConnection);
        });
    }, Qt::QueuedConnection);
}

void AppService::api(const QByteArray &method, const QString &path, const QUrlQuery &query,
                     QJsonObject body, Result result, bool retried)
{
    const auto organization = m_orgGeneration;
    ensureToken([this, method, path, query, body, result, retried, organization](bool ok) {
        if (organization != m_orgGeneration) return;
        if (!ok) { result({}, tr("登录已失效，请重新登录"), 401); return; }
        const auto bytes = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
        request(method, path, query, bytes, body.isEmpty() ? QByteArray() : QByteArray("application/json"), true,
            [this, method, path, query, body, result, retried, organization](QJsonValue value, QString failure, int code) {
                if (organization != m_orgGeneration) return;
                if (code == 401 && !retried && !m_refresh.isEmpty()) {
                    ensureToken([this, method, path, query, body, result, organization](bool fresh) {
                        if (organization != m_orgGeneration) return;
                        if (!fresh) { result({}, tr("登录已失效，请重新登录"), 401); return; }
                        api(method, path, query, body, result, true);
                    }, true);
                    return;
                }
                result(value, failure, code);
            });
    });
}

void AppService::assets(QString category, QString search, int offset, int limit, QString order)
{
    offset = qMax(0, offset); limit = qBound(1, limit, 200);
    QUrlQuery query;
    addQueryItem(query, "search", search); addQueryItem(query, "order", order); addQueryItem(query, "oid", m_org);
    const auto requestId = ++m_assetRequest;
    const bool combined = category == "windows" || category == "other";
    if (!combined) {
        if (category == "database" || category == "device" || category == "web") addQueryItem(query, "category", category);
        else if (category != "favorite" && category != "all") addQueryItem(query, "type", category);
        addQueryItem(query, "offset", QString::number(offset)); addQueryItem(query, "limit", QString::number(limit));
        const QString path = category == "favorite" ? QStringLiteral("/api/v1/perms/users/self/nodes/favorite/assets/") : assetRoot;
        api("GET", path, query, {}, [this, requestId](QJsonValue value, QString failure, int) {
            if (requestId != m_assetRequest) return;
            if (!failure.isEmpty()) { emit error(failure); return; }
            QJsonArray items; int count = 0;
            if (!pageData(value, items, count)) { emit error(tr("资产列表响应格式错误")); return; }
            emit assetsReady(items, count);
        });
        return;
    }
    const QString first = category == "windows" ? "windows" : "unix";
    const QString second = category == "windows" ? "windows_ad" : "other";
    addQueryItem(query, "type", first);
    fetchAssetType(query, 0, {}, requestId, [this, query, second, requestId, offset, limit, order](QJsonArray firstItems, QString failure) mutable {
        if (requestId != m_assetRequest) return;
        if (!failure.isEmpty()) { emit error(failure); return; }
        query.removeAllQueryItems("type"); addQueryItem(query, "type", second);
        fetchAssetType(query, 0, {}, requestId, [this, firstItems, requestId, offset, limit, order](QJsonArray secondItems, QString failure) {
            if (requestId != m_assetRequest) return;
            if (!failure.isEmpty()) { emit error(failure); return; }
            QSet<QString> ids; std::vector<QJsonValue> items;
            for (const auto &list : {firstItems, secondItems}) for (const auto &item : list) {
                const auto id = item.toObject().value("id").toString();
                if (id.isEmpty() || !ids.contains(id)) { ids.insert(id); items.push_back(item); }
            }
            const QString normalized = order.trimmed().isEmpty() ? "name" : order.trimmed();
            const bool descending = normalized.startsWith('-');
            const QString field = descending ? normalized.mid(1) : normalized;
            std::stable_sort(items.begin(), items.end(), [field, descending](QJsonValue a, QJsonValue b) {
                const auto left = a.toObject(), right = b.toObject();
                int comparison = QString::compare(left.value(field).toString().toLower(), right.value(field).toString().toLower());
                if (!comparison) comparison = QString::compare(left.value("id").toString(), right.value("id").toString());
                return descending ? comparison > 0 : comparison < 0;
            });
            QJsonArray page;
            for (int i = offset; i < qMin(static_cast<int>(items.size()), offset + limit); ++i) page.append(items.at(i));
            emit assetsReady(page, static_cast<int>(items.size()));
        });
    });
}

void AppService::fetchAssetType(QUrlQuery query, int offset, QJsonArray accumulated, quint64 requestId,
                                std::function<void(QJsonArray, QString)> done)
{
    if (requestId != m_assetRequest) return;
    query.removeAllQueryItems("offset"); query.removeAllQueryItems("limit");
    addQueryItem(query, "offset", QString::number(offset)); addQueryItem(query, "limit", "200");
    api("GET", assetRoot, query, {}, [this, query, offset, accumulated, requestId, done](QJsonValue value, QString failure, int) mutable {
        if (requestId != m_assetRequest) return;
        if (!failure.isEmpty()) { done({}, failure); return; }
        QJsonArray page; int count = 0;
        if (!pageData(value, page, count)) { done({}, tr("资产列表响应格式错误")); return; }
        for (const auto &item : page) accumulated.append(item);
        if (accumulated.size() > 1000000) { done({}, tr("资产数量超过客户端合并上限，请缩小搜索范围")); return; }
        if (page.isEmpty() || accumulated.size() >= count) { done(accumulated, {}); return; }
        fetchAssetType(query, offset + page.size(), accumulated, requestId, done);
    });
}

void AppService::detail(QString id)
{
    if (id.isEmpty()) return;
    api("GET", assetRoot + pathId(id) + '/', {}, {}, [this, id](QJsonValue value, QString failure, int) {
        if (!failure.isEmpty()) { emit error(failure); return; }
        emit detailReady(id, value.toObject());
    });
}

void AppService::lookupDetail(const QString &id, LookupResult done)
{
    if (id.isEmpty()) { done({}, tr("资产标识为空")); return; }
    api("GET", assetRoot + pathId(id) + '/', {}, {}, [done](QJsonValue value, QString failure, int) {
        if (failure.isEmpty() && !value.isObject()) failure = tr("资产详情响应格式错误");
        done(value, failure);
    });
}

void AppService::lookupMethods(LookupResult done)
{
    const quint64 revision = ++m_methodsLookup;
    emit methodsLookupStarted();
    QUrlQuery query;
#if defined(Q_OS_MACOS)
    addQueryItem(query, "os", "mac");
#elif defined(Q_OS_WIN)
    addQueryItem(query, "os", "windows");
#else
    addQueryItem(query, "os", "linux");
#endif
    api("GET", "/api/v1/terminal/components/connect-methods/", query, {}, [this, done, revision](QJsonValue value, QString failure, int) {
        if (revision != m_methodsLookup) { done({}, methodsSuperseded); return; }
        if (failure.isEmpty() && !value.isObject()) failure = tr("连接方式响应格式错误");
        done(value, failure);
    });
}

void AppService::favorite(QString id, bool enabled)
{
    QUrlQuery query; QJsonObject body;
    if (enabled) body.insert("asset", id); else addQueryItem(query, "asset", id);
    api(enabled ? "POST" : "DELETE", "/api/v1/assets/favorite-assets/", query, body,
        [this, id, enabled](QJsonValue, QString failure, int) {
            emit favoriteFinished(id, enabled, failure.isEmpty());
            if (!failure.isEmpty()) emit error(failure); else emit changed();
        });
}

void AppService::rename(QString id, QString name)
{
    api("POST", "/api/v1/assets/my-asset/", {}, {{"asset", id}, {"name", name}, {"oid", m_org}},
        [this](QJsonValue, QString failure, int) { if (!failure.isEmpty()) emit error(failure); else emit changed(); });
}

void AppService::connection(QJsonObject body, QJsonObject extra)
{
    if (connectionInProgress()) return;
    if (body.value("asset").toString().isEmpty() || body.value("account").toString().isEmpty()
        || body.value("protocol").toString().isEmpty() || body.value("connect_method").toString().isEmpty()) {
        emit connectionFinished(tr("请选择资产、账号、协议和连接方式")); return;
    }
    m_connection.stage = ConnectionStage::Requesting;
    m_connection.body = std::move(body);
    m_connection.extra = std::move(extra);
    requestConnectionToken(false);
}

void AppService::clearConnection()
{
    m_ticketTimer.stop();
    m_connection = {};
    ++m_connectionRevision;
}

void AppService::finishConnection(const QString &failure)
{
    clearConnection();
    emit connectionFinished(failure);
}

void AppService::confirmConnectionApproval()
{
    if (m_connection.stage != ConnectionStage::Confirming) return;
    m_connection.stage = ConnectionStage::Submitting;
    requestConnectionToken(true);
}

void AppService::requestConnectionToken(bool createTicket)
{
    QUrlQuery query;
    if (createTicket) query.addQueryItem("create_ticket", "1");
    auto body = createTicket ? std::exchange(m_connection.body, QJsonObject()) : m_connection.body;
    api("POST", connectionTokenPath, query, std::move(body),
        [this, revision = m_connectionRevision, createTicket](QJsonValue value, QString failure, int status) {
            if (revision != m_connectionRevision) return;
            const auto token = value.toObject();
            const auto code = token.value("code").toString();
            if (status == 400 && code == "acl_reject") {
                finishConnection(tr("访问控制规则拒绝了本次连接。")); return;
            }
            if (status == 400 && code == "acl_review" && !createTicket) {
                m_connection.stage = ConnectionStage::Confirming;
                emit connectionApprovalRequired();
                return;
            }
            m_connection.body = {};
            if (!failure.isEmpty()) {
                if (createTicket && (status == 0 || status >= 500))
                    finishConnection(tr("无法确认审批是否已提交，请先到站点检查工单，避免重复提交：%1").arg(failure));
                else
                    finishConnection((createTicket ? tr("提交审批失败：%1") : tr("创建连接令牌失败：%1")).arg(failure));
                return;
            }
            m_connection.tokenId = token.value("id").toString();
            if (m_connection.tokenId.isEmpty()) {
                finishConnection(tr("站点未返回连接令牌 ID")); return;
            }
            const auto ticket = token.value("from_ticket");
            if (!ticket.isNull() && !ticket.isUndefined()) {
                const auto ticketId = ticket.isObject() ? ticket.toObject().value("id").toString() : ticket.toString();
                static const QRegularExpression uuid(QStringLiteral(
                    "\\A[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\z"));
                if (!uuid.match(ticketId).hasMatch()) {
                    finishConnection(tr("站点返回了无效的审批工单信息，请到站点查看工单。")); return;
                }
                const auto info = token.value("from_ticket_info").toObject();
                const QUrl site(m_site);
                const auto ticketPath = ticketRoot + ticketId + '/';
                m_connection.checkPath = ticketApiPath(info.value("check_ticket_api").toObject(), "GET", ticketPath, site);
                m_connection.closePath = ticketApiPath(info.value("close_ticket_api").toObject(), "PUT", ticketPath + "close/", site);
                if (m_connection.checkPath.isEmpty() || m_connection.closePath.isEmpty()) {
                    finishConnection(tr("站点返回了无效的审批接口，请到站点查看工单。")); return;
                }
                QStringList assignees;
                for (const auto &assignee : info.value("assignees").toArray()) {
                    const auto name = assignee.toString().trimmed();
                    if (!name.isEmpty()) assignees.append(name);
                }
                const auto detail = site.resolved(QUrl(info.value("ticket_detail_page_url").toString(), QUrl::StrictMode));
                const auto detailPath = detail.path(QUrl::FullyEncoded);
                const auto expires = token.value("date_expired").toString();
                m_connection.expiresAt = QDateTime::fromString(expires, Qt::ISODateWithMs);
                if (!m_connection.expiresAt.isValid()) m_connection.expiresAt = QDateTime::fromString(expires, Qt::ISODate);
                m_connection.stage = ConnectionStage::Waiting;
                m_ticketTimer.start(ticketPollIntervalMs);
                emit connectionApprovalWaiting(assignees,
                    sameSiteUrl(detail, site) && (detailPath == "/ui/" || detailPath == site.path(QUrl::FullyEncoded) + "/ui/")
                        ? detail : QUrl(), m_connection.expiresAt);
                return;
            }
            if (token.value("is_active") == QJsonValue(false)) {
                finishConnection(tr("连接令牌未激活，无法继续连接。")); return;
            }
            resolveClientUrl(false);
        });
}

void AppService::pollConnectionApproval()
{
    if (m_connection.stage != ConnectionStage::Waiting || expireConnectionApproval()) return;
    api("GET", m_connection.checkPath, {}, {}, [this, revision = m_connectionRevision](QJsonValue value, QString failure, int status) {
        if (revision != m_connectionRevision || m_connection.stage != ConnectionStage::Waiting || expireConnectionApproval()) return;
        if (!failure.isEmpty()) {
            if ((status == 0 || status >= 500) && ++m_connection.pollFailures < 3)
                m_ticketTimer.start(ticketPollIntervalMs);
            else
                finishConnection(tr("无法继续查询审批状态，请到站点查看工单：%1").arg(failure));
            return;
        }
        m_connection.pollFailures = 0;
        const auto ticket = value.toObject();
        const auto ticketStatus = ticket.value("status").toObject().value("value").toString();
        const auto state = ticket.value("state").toObject().value("value").toString();
        if (ticketStatus == "open" && state == "pending") {
            m_ticketTimer.start(ticketPollIntervalMs); return;
        }
        if (ticketStatus == "closed") {
            if (state == "approved") { resolveClientUrl(true); return; }
            if (state == "rejected") { finishConnection(tr("本次资产连接的审批未通过。")); return; }
            if (state == "closed") { finishConnection(tr("本次资产连接的审批工单已关闭。")); return; }
        }
        finishConnection(tr("无法识别站点返回的审批状态，请到站点查看工单。"));
    });
}

void AppService::cancelConnectionApproval()
{
    if (m_connection.stage == ConnectionStage::Confirming) { finishConnection(); return; }
    closeConnectionApproval(false);
}

bool AppService::expireConnectionApproval()
{
    if (m_connection.stage != ConnectionStage::Waiting || !m_connection.expiresAt.isValid()
        || m_connection.expiresAt > QDateTime::currentDateTimeUtc()) return false;
    closeConnectionApproval(true);
    return true;
}

void AppService::closeConnectionApproval(bool timedOut)
{
    if (m_connection.stage != ConnectionStage::Waiting) return;
    m_connection.stage = ConnectionStage::Canceling;
    m_ticketTimer.stop();
    const auto revision = m_connectionRevision;
    emit connectionApprovalClosing();
    if (revision != m_connectionRevision) return;
    api("PUT", m_connection.closePath, {}, {}, [this, revision, timedOut](QJsonValue, QString failure, int) {
        if (revision != m_connectionRevision) return;
        const auto timeout = timedOut ? tr("审批等待已超时，请重新发起申请。") : QString();
        if (failure.isEmpty()) {
            finishConnection(timeout);
        } else if (timedOut) {
            finishConnection(timeout + QLatin1Char('\n') + tr("未能确认原工单已关闭，请先到站点查看：%1").arg(failure));
        } else {
            finishConnection(tr("已停止等待，但未能确认工单已关闭，请到站点查看：%1").arg(failure));
        }
    });
}

void AppService::resolveClientUrl(bool approved)
{
    m_connection.stage = ConnectionStage::Launching;
    const auto revision = m_connectionRevision;
    if (approved) emit connectionApprovalApproved();
    if (revision != m_connectionRevision) return;
    QUrlQuery query;
    const auto &extra = m_connection.extra;
    for (auto i = extra.begin(); i != extra.end(); ++i) addQueryItem(query, i.key(), i.value().toVariant().toString());
    api("GET", connectionTokenPath + pathId(m_connection.tokenId) + "/client-url/", query, {},
        [this, revision, approved](QJsonValue response, QString failure, int status) {
            if (revision != m_connectionRevision) return;
            if (!failure.isEmpty()) {
                if (approved && (status == 400 || status == 403) && m_connection.expiresAt.isValid()
                    && m_connection.expiresAt <= QDateTime::currentDateTimeUtc())
                    finishConnection(tr("审批已通过，但连接令牌可能已过期。请重新发起连接。"));
                else
                    finishConnection((approved ? tr("审批已通过，但无法获取连接地址：%1") : tr("获取客户端连接地址失败：%1")).arg(failure));
                return;
            }
            const auto url = response.toObject().value("url").toString();
            if (!Launcher::isConnectionUrl(url)) { finishConnection(tr("站点返回了无效客户端连接地址")); return; }
            clearConnection();
            emit clientUrlReady(url);
        });
}

void AppService::metadata()
{
    const std::vector<std::pair<QString, QString>> endpoints{
        {"preferences", "/api/v1/users/preference/"}, {"methods", "/api/v1/terminal/components/connect-methods/"},
        {"version", versionPath}, {"favorites", "/api/v1/assets/favorite-assets/"}};
    for (const auto &endpoint : endpoints) {
        if (endpoint.first == "methods") {
            lookupMethods([this](QJsonValue value, QString failure) {
                if (failure == methodsSuperseded) return;
                if (!failure.isEmpty()) {
                    emit status(tr("部分站点配置加载失败：methods"));
                    emit metadataReady("methods", QJsonValue(QJsonValue::Null));
                } else emit metadataReady("methods", value);
            });
            continue;
        }
        QUrlQuery query; if (endpoint.first == "preferences") addQueryItem(query, "category", "luna");
        const auto completed = [this, endpoint](QJsonValue value, QString failure, int) {
            if (!failure.isEmpty()) {
                emit status(tr("部分站点配置加载失败：%1").arg(endpoint.first));
                emit metadataReady(endpoint.first, QJsonValue(QJsonValue::Null));
                return;
            }
            emit metadataReady(endpoint.first, value);
        };
        if (endpoint.first == "version") request("GET", endpoint.second, query, {}, {}, false, completed);
        else api("GET", endpoint.second, query, {}, completed);
    }
}
