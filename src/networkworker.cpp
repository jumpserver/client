#include "networkworker.h"
#include "platform.h"
#include <QTimer>
#include <QSslError>
#include <QVariant>
#include <QThread>
#include <memory>

NetworkWorker::NetworkWorker(QObject *parent) : QObject(parent) {}

void NetworkWorker::cancelBefore(quint64 generation)
{
    m_generation = generation;
    const auto replies = findChildren<QNetworkReply *>();
    for (auto *reply : replies) {
        if (reply->property("generation").toULongLong() < generation)
            reply->abort();
    }
}

void NetworkWorker::send(QNetworkRequest request, QByteArray method, QByteArray body,
                         QString mode, QString address, quint64 generation, Completion completion)
{
    if (generation < m_generation || QThread::currentThread()->isInterruptionRequested()) return;
    QString failure;
    QList<QNetworkProxy> proxies;
    if (mode == "system") {
        proxies = SystemProxy::resolve(request.url(), &failure);
    } else if (mode == "manual") {
        const QUrl url(address.contains("://") ? address : "http://" + address);
        if (!url.isValid() || url.scheme() != "http" || url.host().isEmpty() ||
            !url.userInfo().isEmpty() || url.port(8080) <= 0 ||
            (url.path() != "" && url.path() != "/") || url.hasQuery() || url.hasFragment()) {
            failure = tr("代理地址无效，请填写 HTTP 代理主机和端口");
        } else {
            proxies.append(QNetworkProxy(QNetworkProxy::HttpProxy, url.host(), quint16(url.port(8080))));
        }
    } else {
        proxies.append(QNetworkProxy(QNetworkProxy::NoProxy));
    }
    if (!failure.isEmpty() || proxies.isEmpty()) {
        NetworkResult result;
        result.failure = failure.isEmpty() ? tr("系统代理解析失败，请检查代理配置") : failure;
        completion(result);
        return;
    }
    sendResolved(request, method, body, proxies, generation, std::move(completion),
                 QDeadlineTimer(30000));
}

void NetworkWorker::sendResolved(QNetworkRequest request, QByteArray method, QByteArray body,
                                 QList<QNetworkProxy> proxies, quint64 generation,
                                 Completion completion, QDeadlineTimer deadline)
{
    if (QThread::currentThread()->isInterruptionRequested()) return;
    auto *manager = new QNetworkAccessManager(this);
    manager->setProxy(proxies.takeFirst());
    connect(manager, &QNetworkAccessManager::sslErrors, manager,
            [](QNetworkReply *reply, const QList<QSslError> &) { reply->ignoreSslErrors(); });
    auto *reply = method == "GET" ? manager->get(request)
                                  : manager->sendCustomRequest(request, method, body);
    reply->setProperty("generation", QVariant::fromValue(generation));
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->start(int(qMax(qint64(1), deadline.remainingTime())));
    connect(timer, &QTimer::timeout, reply, [reply] {
        reply->setProperty("timedOut", true);
        reply->abort();
    });
    auto responseBody = std::make_shared<QByteArray>();
    reply->setReadBufferSize(64 * 1024);
    connect(reply, &QNetworkReply::readyRead, reply, [reply, responseBody] {
        responseBody->append(reply->readAll());
        if (responseBody->size() > 16 * 1024 * 1024) {
            reply->setProperty("oversized", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, manager, generation, completion, responseBody, request, method, body, proxies, deadline] {
        NetworkResult result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.error = reply->error();
        result.body = *responseBody;
        if (reply->isOpen()) result.body.append(reply->readAll());
        if (reply->property("timedOut").toBool()) result.failure = tr("网络请求超时，请检查网络与代理服务");
        if (reply->property("oversized").toBool()) result.failure = tr("站点响应超过大小限制");
        manager->deleteLater();
        if (generation < m_generation) return;
        const bool proxyFailure = result.error >= QNetworkReply::ProxyConnectionRefusedError &&
                                  result.error <= QNetworkReply::UnknownProxyError &&
                                  result.error != QNetworkReply::ProxyAuthenticationRequiredError;
        if (method == "GET" && proxyFailure && result.status == 0 && result.body.isEmpty() &&
            result.failure.isEmpty() && !proxies.isEmpty() && !deadline.hasExpired()) {
            sendResolved(request, method, body, proxies, generation, completion, deadline);
            return;
        }
        completion(result);
    });
}
