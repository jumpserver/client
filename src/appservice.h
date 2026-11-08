#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QThread>
#include "networkworker.h"
#include <functional>
#include <vector>

// All network activity is asynchronous. Tokens never cross the UI boundary.
class AppService : public QObject {
    Q_OBJECT
public:
    using LookupResult = std::function<void(QJsonValue, QString)>;
    explicit AppService(QObject *parent = nullptr);
    ~AppService() override;
    QString site() const { return m_site; }
    QString org() const { return m_org; }
    quint64 contextRevision() const { return m_orgGeneration; }
    quint64 methodsRevision() const { return m_methodsLookup; }
    bool authenticated() const { return !m_access.isEmpty(); }
    bool loginPending() const { return m_loggingIn; }
    void login(QString site, bool loopback = false);
    void cancelLogin();
    void acceptCallback(QUrl url);
    void restore(QString site);
    void logout();
    void setOrg(QString org);
    void assets(QString category, QString search, int offset, int limit, QString order = QString());
    void detail(QString id);
    void lookupDetail(const QString &id, LookupResult done);
    void lookupMethods(LookupResult done);
    void favorite(QString id, bool enabled);
    void rename(QString id, QString name);
    void connection(QJsonObject body, QJsonObject extra = QJsonObject());
    void metadata();
    void applyProxy();
signals:
    void contextChanged();
    void methodsLookupStarted();
    void status(QString message, bool statusOnly = false);
    void error(QString message, bool statusOnly = false);
    void loginStateChanged(bool pending);
    void loggedIn(QJsonObject profile, QJsonValue permissions, QJsonObject currentOrg, QJsonObject publicSettings);
    void loggedOut();
    void assetsReady(QJsonArray assets, int count);
    void detailReady(QString id, QJsonObject detail);
    void metadataReady(QString kind, QJsonValue value);
    void favoriteFinished(QString id, bool enabled, bool success);
    void changed();
    void clientUrlReady(QString url);
    void authorizationUrlReady(QUrl url);
private:
    friend class TestAppService;
    friend class TestUi;
    using Result = std::function<void(QJsonValue, QString, int)>;
    using Ready = std::function<void(bool)>;
    QThread m_networkThread;
    NetworkWorker *m_worker = nullptr;
    QString m_proxyMode, m_proxyAddress;
    QTcpServer m_callback;
    QTimer m_loginTimer;
    QTimer m_sessionTimer;
    QString m_site, m_org, m_access, m_refresh, m_clientId;
    QString m_state, m_verifier, m_redirect;
    qint64 m_expires = 0;
    qint64 m_refreshLifetime = 0, m_refreshExpiresAt = 0;
    quint64 m_generation = 0;
    quint64 m_assetRequest = 0;
    quint64 m_orgGeneration = 0;
    quint64 m_methodsLookup = 0;
    bool m_refreshing = false;
    bool m_loggingIn = false;
    bool m_persistCredentials = true;
    std::vector<Ready> m_refreshWaiters;
    QString credentialKey() const;
    bool configureSite(QString site);
    void resetSession();
    bool checkSessionExpiry();
    void scheduleSessionExpiry();
    void setLoginPending(bool pending);
    void clearPendingLogin();
    void failLogin(const QString &message, bool statusOnly = false);
    void beginAuthorization(bool loopback);
    void persist();
    void loadIdentity();
    bool applyToken(const QJsonObject &token);
    void ensureToken(Ready ready, bool force = false);
    void request(const QByteArray &method, const QString &path, const QUrlQuery &query,
                 const QByteArray &body, const QByteArray &contentType, bool authenticated, Result result);
    void api(const QByteArray &method, const QString &path, const QUrlQuery &query,
             QJsonObject body, Result result, bool retried = false);
    void fetchAssetType(QUrlQuery query, int offset, QJsonArray items, quint64 requestId,
                        std::function<void(QJsonArray, QString)> done);
};
