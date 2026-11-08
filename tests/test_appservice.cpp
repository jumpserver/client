#include "appservice.h"
#include "platform.h"
#include "tls_fixture.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QFile>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpSocket>
#include <QThread>
#include <QtTest>
#include <memory>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Match application/x-www-form-urlencoded decoding used by the server, including '+'.
static QMap<QString, QString> decodeForm(QByteArray bytes)
{
    QMap<QString, QString> result;
    for (auto item : bytes.split('&')) {
        const int equals = item.indexOf('=');
        auto key = equals < 0 ? item : item.left(equals);
        auto value = equals < 0 ? QByteArray() : item.mid(equals + 1);
        key.replace('+', ' '); value.replace('+', ' ');
        result.insert(QUrl::fromPercentEncoding(key), QUrl::fromPercentEncoding(value));
    }
    return result;
}

struct HttpRequest {
    QByteArray method;
    QUrl url;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
};

class MockServer : public QTcpServer {
public:
    explicit MockServer(bool tls) : useTls(tls) {}
    const bool useTls;
};

class HttpMock : public QObject {
public:
    MockServer server;
    QVector<HttpRequest> requests;
    std::function<void(const HttpRequest &, QTcpSocket *)> handler;
    QTemporaryDir tlsFiles;
    qint64 tlsPid = 0;
    QProcess tlsProxy;
    quint16 tlsPort = 0;
    QString startError;
    explicit HttpMock(bool tls = false, bool hostnameMismatch = true) : server(tls)
    {
        if (!server.listen(QHostAddress::LocalHost)) qFatal("Cannot start local test server");
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int split = buffer->indexOf("\r\n\r\n");
                    if (split < 0) return;
                    const auto lines = buffer->left(split).split('\n');
                    const auto first = lines.first().trimmed().split(' ');
                    HttpRequest request; request.method = first.value(0);
                    request.url = QUrl(QString::fromUtf8(first.value(1)));
                    for (int i = 1; i < lines.size(); ++i) {
                        const int colon = lines[i].indexOf(':');
                        if (colon >= 0) request.headers.insert(lines[i].left(colon).toLower(), lines[i].mid(colon + 1).trimmed());
                    }
                    const int length = request.headers.value("content-length").toInt();
                    if (buffer->size() < split + 4 + length) return;
                    request.body = buffer->mid(split + 4, length);
                    socket->disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
                    requests.append(request);
                    if (handler) handler(request, socket); else reply(socket, QJsonObject{});
                });
            }
        });
        if (tls) {
            // Exercise Qt's real TLS client without Secure Transport's legacy
            // server-key import/signing path (which blocks in macOS securityd).
            const auto writeFixture = [&](const QString &name, const QByteArray &bytes) {
                QFile file(tlsFiles.filePath(name));
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
                    startError = QStringLiteral("Cannot write synthetic TLS fixture %1 (%2)").arg(name, file.errorString());
                    return false;
                }
                return true;
            };
            if (writeFixture("certificate.pem", hostnameMismatch ? testTlsCertificatePem : testTlsIpCertificatePem)
                && writeFixture("key.pem", testTlsPrivateKeyPem))
                startTlsProxy();
        }
    }
    ~HttpMock() override {
#ifdef Q_OS_UNIX
        if (tlsPid > 0) {
            kill(static_cast<pid_t>(tlsPid), SIGTERM);
            waitpid(static_cast<pid_t>(tlsPid), nullptr, 0);
        }
#else
        if (tlsProxy.state() != QProcess::NotRunning) {
            tlsProxy.terminate();
            if (!tlsProxy.waitForFinished(3000)) tlsProxy.kill();
        }
#endif
    }
    QString url() const
    {
        return QStringLiteral("%1://127.0.0.1:%2").arg(server.useTls ? "https" : "http").arg(server.useTls ? tlsPort : server.serverPort());
    }
    // A separate interpreter can take well over the 5 seconds this used to allow.
    // Report why it stayed silent instead of aborting the whole test binary.
    void startTlsProxy()
    {
#ifdef Q_OS_UNIX
        startTlsProxyUnix();
#else
        startTlsProxyProcess();
#endif
    }
#ifdef Q_OS_UNIX
    void startTlsProxyUnix()
    {
        static constexpr int StartupLimitMs = 30000;
        const QString portPath = tlsFiles.filePath("port");
        QFile::remove(portPath);
        const pid_t pid = fork();
        if (pid < 0) {
            startError = QStringLiteral("Cannot fork local TLS fixture (%1)").arg(qt_error_string());
            return;
        }
        if (pid == 0) {
            const QByteArray program = QByteArray(TEST_PYTHON_EXECUTABLE);
            const QByteArray script = QByteArray(TEST_TLS_PROXY);
            const QByteArray upstream = QByteArray::number(server.serverPort());
            const QByteArray certificate = tlsFiles.filePath("certificate.pem").toUtf8();
            const QByteArray key = tlsFiles.filePath("key.pem").toUtf8();
            const QByteArray port = portPath.toUtf8();
            char *arguments[] = {const_cast<char *>(program.constData()),
                                 const_cast<char *>("-I"), const_cast<char *>("-S"),
                                 const_cast<char *>("-B"),
                                 const_cast<char *>(script.constData()),
                                 const_cast<char *>("--upstream"), const_cast<char *>(upstream.constData()),
                                 const_cast<char *>("--certificate"), const_cast<char *>(certificate.constData()),
                                 const_cast<char *>("--key"), const_cast<char *>(key.constData()),
                                 const_cast<char *>("--port-file"), const_cast<char *>(port.constData()),
                                 nullptr};
            unsetenv("PYTHONEXECUTABLE");
            unsetenv("PYTHONHOME");
            unsetenv("PYTHONPATH");
            setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
            execv(program.constData(), arguments);
            _exit(127);
        }
        tlsPid = pid;
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < StartupLimitMs) {
            QFile portFile(portPath);
            if (portFile.open(QIODevice::ReadOnly)) {
                tlsPort = portFile.readAll().trimmed().toUShort();
                if (tlsPort) return;
            }
            int status = 0;
            const pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                tlsPid = 0;
                startError = QStringLiteral("Local TLS fixture exited before reporting its port (code=%1, signal=%2)")
                    .arg(WIFEXITED(status) ? WEXITSTATUS(status) : -1)
                    .arg(WIFSIGNALED(status) ? WTERMSIG(status) : 0);
                return;
            }
            QThread::msleep(50);
        }
        startError = QStringLiteral("Local TLS fixture did not report its port within %1 ms").arg(StartupLimitMs);
    }
#else
    void startTlsProxyProcess()
    {
        static constexpr int StartupLimitMs = 30000;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("PYTHONEXECUTABLE"));
        environment.remove(QStringLiteral("PYTHONHOME"));
        environment.remove(QStringLiteral("PYTHONPATH"));
        environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
        tlsProxy.setProcessEnvironment(environment);
        const QString portPath = tlsFiles.filePath("port");
        QFile::remove(portPath);
        tlsProxy.start(QString::fromUtf8(TEST_PYTHON_EXECUTABLE),
            {QStringLiteral("-I"), QStringLiteral("-S"), QStringLiteral("-B"),
             QString::fromUtf8(TEST_TLS_PROXY), "--upstream", QString::number(server.serverPort()),
             "--certificate", tlsFiles.filePath("certificate.pem"), "--key", tlsFiles.filePath("key.pem"),
             "--port-file", portPath});
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < StartupLimitMs) {
            QFile portFile(portPath);
            if (portFile.open(QIODevice::ReadOnly)) {
                tlsPort = portFile.readAll().trimmed().toUShort();
                if (tlsPort) return;
            }
            QThread::msleep(50);
        }
        startError = QStringLiteral("Local TLS fixture did not report its port within %1 ms "
                                    "(process=%2, startError=%3, stderr=%4)")
            .arg(StartupLimitMs).arg(static_cast<int>(tlsProxy.state())).arg(tlsProxy.errorString())
            .arg(QString::fromUtf8(tlsProxy.readAllStandardError()).simplified());
    }
#endif
    static void reply(QTcpSocket *socket, QJsonValue value, int status = 200)
    {
        const QByteArray body = value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                              : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};

class TestAppService : public QObject {
    Q_OBJECT
private:
    void session(AppService &service, HttpMock &server)
    {
        service.m_site = server.url(); service.m_access = "test-access";
        service.m_org = "test-org"; service.m_persistCredentials = false;
    }
public slots:
    void browser(QUrl) {} // OAuth tests intercept URLs without opening a real browser.
private slots:
    void initTestCase()
    {
        qRegisterMetaType<QJsonArray>(); qRegisterMetaType<QJsonObject>(); qRegisterMetaType<QJsonValue>();
        QDesktopServices::setUrlHandler("http", this, "browser");
        QDesktopServices::setUrlHandler("https", this, "browser");
    }
    void cleanupTestCase()
    {
        QDesktopServices::unsetUrlHandler("http"); QDesktopServices::unsetUrlHandler("https");
    }
    void httpsLoginIgnoresCertificateValidation_data()
    {
        QTest::addColumn<bool>("hostnameMismatch");
        QTest::newRow("self-signed") << false;
        QTest::newRow("self-signed-and-wrong-hostname") << true;
    }
    void httpsLoginIgnoresCertificateValidation()
    {
        QFETCH(bool, hostnameMismatch);
        QVERIFY2(QSslSocket::supportsSsl(), "Tests require a functional Qt TLS backend");
        QVERIFY(!QSslCertificate(QByteArray(testTlsCertificatePem)).isNull());
        QVERIFY(!QSslCertificate(QByteArray(testTlsIpCertificatePem)).isNull());
        QVERIFY(!QSslKey(QByteArray(testTlsPrivateKeyPem), QSsl::Rsa).isNull());
        HttpMock mock(true, hostnameMismatch);
        QVERIFY2(mock.startError.isEmpty(), qPrintable(mock.startError));
        AppService service; service.m_persistCredentials = false;

        // First prove the exact server certificate is rejected by ordinary Qt
        // defaults. AppService must not modify process-global trust settings.
        QNetworkAccessManager ordinaryNetwork;
        QList<QSslError::SslError> verificationErrors;
        auto *rejected = ordinaryNetwork.get(QNetworkRequest(QUrl(mock.url())));
        connect(rejected, &QNetworkReply::sslErrors, this, [&](const QList<QSslError> &errors) {
            for (const auto &error : errors) verificationErrors.append(error.error());
        });
        QSignalSpy rejectedFinished(rejected, &QNetworkReply::finished);
        QTRY_COMPARE(rejectedFinished.size(), 1);
        QVERIFY2(rejected->error() == QNetworkReply::SslHandshakeFailedError, qPrintable(rejected->errorString()));
        QVERIFY(!verificationErrors.isEmpty());
        QVERIFY(verificationErrors.contains(QSslError::SelfSignedCertificate)
                || verificationErrors.contains(QSslError::SelfSignedCertificateInChain)
                || verificationErrors.contains(QSslError::CertificateUntrusted)
                || verificationErrors.contains(QSslError::UnableToGetLocalIssuerCertificate));
        QCOMPARE(verificationErrors.contains(QSslError::HostNameMismatch), hostnameMismatch);
        QCOMPARE(mock.requests.size(), 0);
        rejected->deleteLater();

        QSignalSpy authorization(&service, &AppService::authorizationUrlReady);
        QSignalSpy loggedIn(&service, &AppService::loggedIn), errors(&service, &AppService::error);
        QSignalSpy assets(&service, &AppService::assetsReady), details(&service, &AppService::detailReady);
        QSignalSpy metadata(&service, &AppService::metadataReady);
        int versions = 0, tokens = 0, authenticatedRequests = 0, revocations = 0;
        mock.handler = [&](const HttpRequest &request, QTcpSocket *socket) {
                QVERIFY(mock.tlsPort != 0);
            const auto path = request.url.path();
            if (path.endsWith("/versions/")) {
                ++versions;
                QVERIFY(!request.headers.contains("authorization"));
                HttpMock::reply(socket, QJsonArray{"4.1.2"});
            } else if (path.endsWith("oauth-authorization-server")) {
                HttpMock::reply(socket, QJsonObject{{"client_id", "tls-client"}});
            } else if (path.endsWith("/token/")) {
                const auto form = decodeForm(request.body);
                QCOMPARE(form.value("grant_type"), tokens == 0 ? QString("authorization_code") : QString("refresh_token"));
                if (tokens == 0) QCOMPARE(form.value("code"), QString("tls-code"));
                else QCOMPARE(form.value("refresh_token"), QString("tls-refresh"));
                ++tokens;
                HttpMock::reply(socket, QJsonObject{{"access_token", "tls-access"}, {"refresh_token", "tls-refresh"}, {"expires_in", 3600}});
            } else if (path.endsWith("/revoke/")) {
                ++revocations;
                QCOMPARE(decodeForm(request.body).value("token"), QString("tls-refresh"));
                HttpMock::reply(socket, QJsonObject{});
            } else {
                ++authenticatedRequests;
                QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer tls-access"));
                if (path == "/api/v1/users/profile/") HttpMock::reply(socket, QJsonObject{{"id", "tls-user"}});
                else if (path == "/api/v1/orgs/orgs/current/") HttpMock::reply(socket, QJsonObject{{"id", "tls-org"}});
                else if (path == "/api/v1/perms/users/self/assets/") {
                    QCOMPARE(request.headers.value("x-jms-org"), QByteArray("tls-org"));
                    HttpMock::reply(socket, QJsonObject{{"count", 1}, {"results", QJsonArray{QJsonObject{{"id", "tls-asset"}}}}});
                } else if (path == "/api/v1/perms/users/self/assets/denied/") {
                    HttpMock::reply(socket, QJsonObject{}, 403);
                } else HttpMock::reply(socket, QJsonObject{});
            }
        };
        service.login(mock.url());
        QTRY_COMPARE(authorization.size(), 1);
        const auto state = QUrlQuery(authorization.first().first().toUrl()).queryItemValue("state");
        service.acceptCallback(QUrl("jms://auth/callback?code=tls-code&state=" + state));
        QTRY_COMPARE(loggedIn.size(), 1);
        // loadIdentity also launches metadata; wait for all four replies before
        // forcing expiration, so event-loop timing cannot change these counts.
        QTRY_COMPARE(metadata.size(), 5); // Version preflight plus four metadata endpoints.
        QVERIFY(service.authenticated()); QVERIFY(!service.loginPending());
        QCOMPARE(versions, 2); QCOMPARE(tokens, 1); QCOMPARE(authenticatedRequests, 7);
        service.m_expires = 1; // Also exercise refresh over the same untrusted TLS endpoint.
        service.assets("linux", "", 0, 20);
        QTRY_COMPARE(assets.size(), 1);
        QCOMPARE(tokens, 2); QCOMPARE(assets.first().at(1).toInt(), 1);
        QCOMPARE(assets.first().first().value<QJsonArray>().first().toObject().value("id").toString(), QString("tls-asset"));
        QCOMPARE(errors.size(), 0);
        service.detail("denied"); // Certificate bypass must not bypass HTTP authorization.
        QTRY_COMPARE(errors.size(), 1); QCOMPARE(details.size(), 0);
        service.logout(); QTRY_COMPARE(revocations, 1);
        QVERIFY(!service.authenticated());
    }
    void invalidTlsHandshakeStillFailsLogin()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "Tests require a functional Qt TLS backend");
        QTcpServer plainServer;
        QVERIFY(plainServer.listen(QHostAddress::LocalHost));
        connect(&plainServer, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = plainServer.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                // Deliberately return valid HTTP/JSON without performing TLS.
                HttpMock::reply(socket, QJsonArray{"4.1.2"});
            }
        });
        AppService service; service.m_persistCredentials = false;
        QSignalSpy errors(&service, &AppService::error), authorization(&service, &AppService::authorizationUrlReady);
        QSignalSpy metadata(&service, &AppService::metadataReady);
        service.login(QStringLiteral("https://127.0.0.1:%1").arg(plainServer.serverPort()));
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!service.loginPending()); QVERIFY(!service.authenticated());
        QVERIFY(!errors.first().first().toString().isEmpty());
        QCOMPARE(authorization.size(), 0); QCOMPARE(metadata.size(), 1);
        QVERIFY(metadata.first().at(1).value<QJsonValue>().isNull());
    }
    void loginVersionPreflight_data()
    {
        QTest::addColumn<QJsonValue>("versions");
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("compatible") << QJsonValue(QJsonArray{"4.1.2"}) << 200 << true;
        QTest::newRow("prefix-and-patch") << QJsonValue(QJsonArray{" v4.1.99 ", "5.0.0"}) << 200 << true;
        QTest::newRow("strip-nondigits") << QJsonValue(QJsonArray{"release4.minor1.patch99"}) << 200 << true;
        QTest::newRow("incompatible-minor") << QJsonValue(QJsonArray{"4.2.0", "3.1.2"}) << 200 << false;
        QTest::newRow("all-digits-in-minor") << QJsonValue(QJsonArray{"v4.1beta2.0"}) << 200 << false;
        QTest::newRow("missing-endpoint") << QJsonValue(QJsonArray{}) << 404 << false;
        QTest::newRow("empty-list") << QJsonValue(QJsonArray{}) << 200 << true;
        QTest::newRow("unavailable") << QJsonValue(QJsonArray{}) << 503 << false;
        QTest::newRow("invalid-shape") << QJsonValue(QJsonObject{}) << 200 << false;
    }
    void loginVersionPreflight()
    {
        QFETCH(QJsonValue, versions);
        QFETCH(int, httpStatus);
        QFETCH(bool, allowed);
        HttpMock mock; AppService service; service.m_persistCredentials = false;
        mock.handler = [&](const HttpRequest &request, QTcpSocket *socket) {
            QVERIFY(!request.headers.contains("authorization"));
            QCOMPARE(request.method, QByteArray("GET"));
            QVERIFY(request.body.isEmpty());
            QVERIFY(!request.headers.contains("content-length"));
            QVERIFY(!request.headers.contains("transfer-encoding"));
            if (request.url.path() == "/api/v1/settings/client/versions/") HttpMock::reply(socket, versions, httpStatus);
            else {
                QVERIFY(allowed);
                QCOMPARE(request.url.path(), QString("/core/auth/oauth2-provider/.well-known/oauth-authorization-server"));
                HttpMock::reply(socket, QJsonObject{{"client_id", "client"}});
            }
        };
        QSignalSpy authorization(&service, &AppService::authorizationUrlReady), errors(&service, &AppService::error);
        QSignalSpy metadata(&service, &AppService::metadataReady);
        service.login(mock.url());
        if (allowed) {
            QTRY_COMPARE(authorization.size(), 1); QCOMPARE(errors.size(), 0);
            QVERIFY(service.loginPending()); QCOMPARE(mock.requests.size(), 2);
            service.cancelLogin();
        } else {
            QTRY_COMPARE(errors.size(), 1); QCOMPARE(authorization.size(), 0);
            QVERIFY(!service.loginPending()); QCOMPARE(mock.requests.size(), 1);
            QVERIFY(errors.first().first().toString().contains(QStringLiteral("版本")));
            QVERIFY(errors.first().at(1).toBool()); // Version failures belong in the status bar.
        }
        QCOMPARE(metadata.size(), 1); QCOMPARE(metadata.first().first().toString(), QString("version"));
        QCOMPARE(metadata.first().at(1).value<QJsonValue>(), httpStatus == 200 ? versions : QJsonValue(QJsonValue::Null));
    }
    void networkFailureFailsVersionPreflight()
    {
        HttpMock mock; AppService service;
        mock.handler = [](const HttpRequest &, QTcpSocket *socket) { socket->abort(); };
        QSignalSpy errors(&service, &AppService::error), authorization(&service, &AppService::authorizationUrlReady);
        QSignalSpy metadata(&service, &AppService::metadataReady);
        service.login(mock.url());
        QTRY_COMPARE(errors.size(), 1); QVERIFY(!service.loginPending());
        QCOMPARE(authorization.size(), 0); QCOMPARE(metadata.size(), 1);
        QVERIFY(metadata.first().at(1).value<QJsonValue>().isNull());
    }
    void oauthPKCEAndState_data()
    {
        QTest::addColumn<QString>("separator");
        QTest::addColumn<bool>("codeFirst");
        QTest::newRow("standard") << QString("&") << false;
        QTest::newRow("html-escaped-code") << QString("&amp;") << false;
        QTest::newRow("html-escaped-state") << QString("&amp;") << true;
    }
    void oauthPKCEAndState()
    {
        QFETCH(QString, separator);
        QFETCH(bool, codeFirst);
        HttpMock mock; AppService service;
        service.m_persistCredentials = false;
        QSignalSpy authorization(&service, &AppService::authorizationUrlReady);
        QSignalSpy loggedIn(&service, &AppService::loggedIn);
        QSignalSpy errors(&service, &AppService::error);
        QSignalSpy loginState(&service, &AppService::loginStateChanged);
        QByteArray verifier;
        const QString clientId = QStringLiteral("client+literal%2B &中文");
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            const QString path = r.url.path();
            if (path.endsWith("/versions/")) HttpMock::reply(socket, QJsonArray{"4.1.2"});
            else if (path.endsWith("oauth-authorization-server")) HttpMock::reply(socket, QJsonObject{{"client_id", clientId}});
            else if (path.endsWith("/token/")) {
                QCOMPARE(r.headers.value("content-type"), QByteArray("application/x-www-form-urlencoded"));
                const auto body = decodeForm(r.body);
                verifier = body.value("code_verifier").toLatin1();
                QCOMPARE(body.value("grant_type"), QString("authorization_code"));
                QCOMPARE(body.value("client_id"), clientId);
                QCOMPARE(body.value("redirect_uri"), QString("jms://auth/callback"));
                QCOMPARE(body.value("code"), QString("test+code%2B &=中文"));
                HttpMock::reply(socket, QJsonObject{{"access_token", "test-access"}, {"refresh_token", "test-refresh"}, {"expires_in", 3600}});
            } else if (path == "/api/v1/users/profile/") HttpMock::reply(socket, QJsonObject{{"id", "user"}, {"username", "tester"}});
            else if (path == "/api/v1/users/profile/permissions/") HttpMock::reply(socket, QJsonObject{{"orgs", QJsonArray{QJsonObject{{"id", "org"}}}}});
            else if (path == "/api/v1/orgs/orgs/current/") HttpMock::reply(socket, QJsonObject{{"id", "org"}});
            else HttpMock::reply(socket, QJsonObject{});
        };
        QVERIFY(!service.loginPending());
        service.login(mock.url());
        QVERIFY(service.loginPending());
        QCOMPARE(loginState.size(), 1); QCOMPARE(loginState.first().first().toBool(), true);
        QTRY_COMPARE(authorization.size(), 1);
        const auto authUrl = authorization.first().at(0).toUrl();
        const QUrlQuery query(authUrl);
        QCOMPARE(decodeForm(authUrl.query(QUrl::FullyEncoded).toUtf8()).value("client_id"), clientId);
        QCOMPARE(decodeForm(authUrl.query(QUrl::FullyEncoded).toUtf8()).value("scope"), QString("write read"));
        QCOMPARE(query.queryItemValue("code_challenge_method"), QString("S256"));
        QVERIFY(query.queryItemValue("state").size() >= 43);
        service.acceptCallback(QUrl("jms://auth/callback?code=bad"));
        service.acceptCallback(QUrl("jms://auth/callback?code=bad&state=wrong"));
        service.acceptCallback(QUrl("jms://auth/callback?code=bad&amp;state=wrong"));
        service.acceptCallback(QUrl("jms://auth/callback?code=bad&state=" + query.queryItemValue("state") + "&amp;state=wrong"));
        QCOMPARE(errors.size(), 4);
        QVERIFY(service.loginPending()); QCOMPARE(loginState.size(), 1);
        QCOMPARE(mock.requests.size(), 2);
        const QString stateField = "state=" + query.queryItemValue("state");
        const QString codeField = "code=test%2Bcode%252B+%26%3D%E4%B8%AD%E6%96%87";
        const QUrl callback("jms://auth/callback?" + (codeFirst ? codeField + separator + stateField
                                                              : stateField + separator + codeField));
        service.acceptCallback(callback);
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(!service.loginPending());
        QCOMPARE(loginState.size(), 2); QCOMPARE(loginState.last().first().toBool(), false);
        QVERIFY(service.authenticated()); QCOMPARE(service.org(), QString("org"));
        const auto challenge = QCryptographicHash::hash(verifier, QCryptographicHash::Sha256).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        QCOMPARE(query.queryItemValue("code_challenge").toLatin1(), challenge);
    }
    void mergedAssetsSortDeduplicateAndPage()
    {
        HttpMock mock; AppService service; session(service, mock);
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer test-access"));
            QCOMPARE(r.headers.value("x-jms-org"), QByteArray("test-org"));
            const auto type = QUrlQuery(r.url).queryItemValue("type");
            QJsonArray assets;
            if (type == "windows") assets = {QJsonObject{{"id", "c"}, {"name", "Charlie"}}, QJsonObject{{"id", "b"}, {"name", "Bravo"}}};
            else if (type == "windows_ad") assets = {QJsonObject{{"id", "a"}, {"name", "Alpha"}}, QJsonObject{{"id", "b"}, {"name", "Bravo"}}};
            else QFAIL("Unexpected category");
            HttpMock::reply(socket, QJsonObject{{"results", assets}, {"count", assets.size()}});
        };
        QSignalSpy assets(&service, &AppService::assetsReady);
        service.assets("windows", "", 1, 1, "name");
        QTRY_COMPARE(assets.size(), 1);
        QCOMPARE(assets.first().at(1).toInt(), 3);
        QCOMPARE(assets.first().at(0).value<QJsonArray>().first().toObject().value("id").toString(), QString("b"));
        QCOMPARE(mock.requests.size(), 2);
    }
    void supersededMergedSearchStopsPagination()
    {
        HttpMock mock; AppService service; session(service, mock);
        QPointer<QTcpSocket> held;
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            if (QUrlQuery(r.url).queryItemValue("search") == "old") held = socket;
            else HttpMock::reply(socket, QJsonObject{{"results", QJsonArray{}}, {"count", 0}});
        };
        QSignalSpy assets(&service, &AppService::assetsReady);
        service.assets("windows", "old", 0, 20);
        QTRY_VERIFY(!held.isNull());
        service.assets("linux", "new", 0, 20);
        QTRY_COMPARE(assets.size(), 1);
        HttpMock::reply(held, QJsonObject{{"results", QJsonArray{QJsonObject{{"id", "stale"}}}}, {"count", 500}});
        QTest::qWait(100);
        QCOMPARE(mock.requests.size(), 2);
        QCOMPARE(assets.size(), 1);
    }
    void searchPreservesFormSpecialCharacters()
    {
        HttpMock mock; AppService service; session(service, mock);
        const QString search = QStringLiteral("C++ %2B &a=b /中文");
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            const auto query = decodeForm(r.url.query(QUrl::FullyEncoded).toUtf8());
            QCOMPARE(query.value("search"), search);
            QCOMPARE(query.value("oid"), QString("test-org"));
            QCOMPARE(r.headers.value("referer"), mock.url().toUtf8());
            QVERIFY(QRegularExpression("^[+-][0-9]{2}:[0-9]{2}$").match(QString::fromLatin1(r.headers.value("x-tz"))).hasMatch());
            HttpMock::reply(socket, QJsonObject{{"results", QJsonArray{}}, {"count", 0}});
        };
        QSignalSpy assets(&service, &AppService::assetsReady);
        service.assets("linux", search, 0, 20); QTRY_COMPARE(assets.size(), 1);
    }
    void connectionAndScopedMutation_data()
    {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("method");
        QTest::addColumn<QString>("endpointProtocol");
        QTest::newRow("ssh") << QString("ssh") << QString("ssh_client") << QString("ssh");
        QTest::newRow("http-applet-over-rdp") << QString("http") << QString("chrome") << QString("rdp");
        QTest::newRow("http-virtualapp-client") << QString("http") << QString("chrome_app") << QString("vnc");
    }
    void connectionAndScopedMutation()
    {
        QFETCH(QString, protocol);
        QFETCH(QString, method);
        QFETCH(QString, endpointProtocol);
        const QJsonObject connectOptions{{"charset", "default"}, {"disableautohash", false},
            {"token_reusable", false}, {"resolution", "auto"}, {"backspaceAsCtrlH", false},
            {"appletConnectMethod", method == "chrome_app" ? "web" : "client"},
            {"virtualappConnectMethod", method == "chrome_app" ? "client" : "web"},
            {"reusable", false}, {"rdp_connection_speed", "auto"}};
        HttpMock mock; AppService service; session(service, mock);
        bool bodyVerified = false;
        QJsonObject payload{{"protocol", endpointProtocol}, {"command", protocol == "http" ? "" : "aBc/+="}};
        if (endpointProtocol == "rdp")
            payload["file"] = QJsonObject{{"name", "synthetic.rdp"},
                {"content", "remoteapplicationmode:i:1\r\nremoteapplicationprogram:s:||chrome\r\n"}};
        if (endpointProtocol == "vnc") {
            payload["endpoint"] = QJsonObject{{"host", "127.0.0.1"}, {"port", 5999}};
            payload["token"] = QJsonObject{{"id", "synthetic-user"}, {"value", "synthetic-value"}};
        }
        const auto connectionUrl = QStringLiteral("jms://")
            + QString::fromLatin1(QJsonDocument(payload).toJson(QJsonDocument::Compact).toBase64());
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            QCOMPARE(r.headers.value("x-jms-org"), QByteArray("test-org"));
            if (r.url.path() == "/api/v1/authentication/connection-token/") {
                const auto body = QJsonDocument::fromJson(r.body).object();
                QCOMPARE(body.value("account").toString(), QString("@INPUT"));
                QCOMPARE(body.value("input_username").toString(), QString("root"));
                QCOMPARE(body.value("protocol").toString(), protocol);
                QCOMPARE(body.value("connect_method").toString(), method);
                if (protocol == "http") QCOMPARE(body["connect_options"].toObject(), connectOptions);
                bodyVerified = true;
                HttpMock::reply(socket, QJsonObject{{"id", "connection"}}, 201);
            } else if (r.url.path().endsWith("/client-url/")) {
                QCOMPARE(QUrlQuery(r.url).queryItemValue("full_screen"), QString("1"));
                QCOMPARE(QUrlQuery(r.url).queryItemValue("rdp_smart_size"), QString("0"));
                QCOMPARE(QUrlQuery(r.url).queryItemValue("rdp_color_quality"), QString("32"));
                HttpMock::reply(socket, QJsonObject{{"url", connectionUrl}});
            } else if (r.url.path() == "/api/v1/assets/favorite-assets/") {
                QCOMPARE(r.method, QByteArray("DELETE"));
                QCOMPARE(QUrlQuery(r.url).queryItemValue("asset"), QString("asset"));
                HttpMock::reply(socket, QJsonObject{});
            } else QFAIL("Unexpected endpoint");
        };
        QSignalSpy clientUrl(&service, &AppService::clientUrlReady), changed(&service, &AppService::changed);
#ifndef Q_OS_WIN
        QTemporaryDir launcherFiles;
        QFile helper(launcherFiles.filePath("helper"));
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("#!/bin/sh\nprintf '%s' \"$1\" > received-url\n");
        helper.close();
        QVERIFY(helper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        Launcher launcher(nullptr, helper.fileName(), launcherFiles.filePath("config.json"));
        QSignalSpy launched(&launcher, &Launcher::finished), launchErrors(&launcher, &Launcher::failed);
        connect(&service, &AppService::clientUrlReady, &launcher, &Launcher::launch);
#endif
        service.connection({{"asset", "asset"}, {"account", "@INPUT"}, {"input_username", "root"}, {"input_secret", "test-only"}, {"protocol", protocol}, {"connect_method", method},
                            {"connect_options", connectOptions}}, {{"full_screen", "1"}, {"rdp_smart_size", "0"}, {"rdp_color_quality", "32"}});
        QTRY_COMPARE(clientUrl.size(), 1); QVERIFY(bodyVerified);
        QCOMPARE(clientUrl.first().first().toString(), connectionUrl);
#ifndef Q_OS_WIN
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launchErrors.size(), 0);
        QFile received(launcherFiles.filePath("received-url"));
        QVERIFY(received.open(QIODevice::ReadOnly));
        QCOMPARE(received.readAll(), connectionUrl.toUtf8());
#endif
        service.favorite("asset", false); QTRY_COMPARE(changed.size(), 1);
    }
    void protocolMenuLookups()
    {
        HttpMock mock;
        AppService service;
        session(service, mock);
        bool detailDone = false, methodsDone = false;
        mock.handler = [&](const HttpRequest &request, QTcpSocket *socket) {
            if (request.url.path() == "/api/v1/terminal/components/connect-methods/") {
#if defined(Q_OS_MACOS)
                QCOMPARE(QUrlQuery(request.url).queryItemValue("os"), QString("mac"));
#elif defined(Q_OS_WIN)
                QCOMPARE(QUrlQuery(request.url).queryItemValue("os"), QString("windows"));
#else
                QCOMPARE(QUrlQuery(request.url).queryItemValue("os"), QString("linux"));
#endif
                HttpMock::reply(socket, QJsonObject{{"ssh", QJsonArray{QJsonObject{{"type", "native"}, {"value", "ssh_client"}}}}});
            } else {
                QVERIFY(request.url.path().endsWith("/asset/"));
                HttpMock::reply(socket, QJsonObject{{"permed_protocols", QJsonArray{QJsonObject{{"name", "ssh"}}}}});
            }
        };
        service.lookupDetail("asset", [&](QJsonValue value, QString error) {
            QVERIFY(error.isEmpty());
            QCOMPARE(value.toObject()["permed_protocols"].toArray().size(), 1);
            detailDone = true;
        });
        service.lookupMethods([&](QJsonValue value, QString error) {
            QVERIFY(error.isEmpty());
            QCOMPARE(value.toObject()["ssh"].toArray().size(), 1);
            methodsDone = true;
        });
        QTRY_VERIFY(detailDone && methodsDone);
    }
    void protocolMenuLookupDiscardedOnOrganizationChange()
    {
        HttpMock mock;
        AppService service;
        session(service, mock);
        mock.handler = [&](const HttpRequest &, QTcpSocket *socket) {
            HttpMock::reply(socket, QJsonObject{{"permed_protocols", QJsonArray{}}});
        };
        bool delivered = false;
        service.lookupDetail("asset", [&](QJsonValue, QString) { delivered = true; });
        service.setOrg("new-org");
        service.setOrg("test-org"); // Returning to the same scope must not revive the old response.
        QTRY_COMPARE(mock.requests.size(), 1);
        QTest::qWait(50);
        QVERIFY(!delivered);
    }
    void protocolMethodsNewestRequestWins()
    {
        HttpMock mock;
        AppService service;
        session(service, mock);
        QPointer<QTcpSocket> first;
        mock.handler = [&](const HttpRequest &, QTcpSocket *socket) {
            if (!first) { first = socket; return; }
            HttpMock::reply(socket, QJsonObject{{"new", QJsonArray{}}});
        };
        bool oldDone = false, newDone = false;
        service.lookupMethods([&](QJsonValue value, QString error) {
            QVERIFY(!error.isEmpty());
            QVERIFY(value.isNull());
            oldDone = true;
        });
        QTRY_VERIFY(first);
        service.lookupMethods([&](QJsonValue value, QString error) {
            QVERIFY(error.isEmpty());
            QVERIFY(value.toObject().contains("new"));
            newDone = true;
        });
        QTRY_VERIFY(newDone);
        HttpMock::reply(first, QJsonObject{{"old", QJsonArray{}}});
        QTRY_VERIFY(oldDone);
    }
    void favoriteCompletion_data()
    {
        QTest::addColumn<bool>("enabled");
        QTest::addColumn<bool>("success");
        QTest::newRow("add") << true << true;
        QTest::newRow("remove") << false << true;
        QTest::newRow("add-rejected") << true << false;
        QTest::newRow("remove-rejected") << false << false;
    }
    void favoriteCompletion()
    {
        QFETCH(bool, enabled);
        QFETCH(bool, success);
        HttpMock mock;
        AppService service;
        session(service, mock);
        mock.handler = [&](const HttpRequest &request, QTcpSocket *socket) {
            QCOMPARE(request.url.path(), QString("/api/v1/assets/favorite-assets/"));
            QCOMPARE(request.method, enabled ? QByteArray("POST") : QByteArray("DELETE"));
            QCOMPARE(request.headers.value("x-jms-org"), QByteArray("test-org"));
            const QString id = enabled ? QJsonDocument::fromJson(request.body).object()["asset"].toString()
                                       : QUrlQuery(request.url).queryItemValue("asset");
            QCOMPARE(id, QString("favorite-asset"));
            HttpMock::reply(socket, QJsonObject{}, success ? 200 : 403);
        };
        QSignalSpy finished(&service, &AppService::favoriteFinished);
        QSignalSpy changed(&service, &AppService::changed), errors(&service, &AppService::error);
        service.favorite("favorite-asset", enabled);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QString("favorite-asset"));
        QCOMPARE(finished.first().at(1).toBool(), enabled);
        QCOMPARE(finished.first().at(2).toBool(), success);
        QCOMPARE(changed.size(), success ? 1 : 0);
        QCOMPARE(errors.size(), success ? 0 : 1);
    }
    void connectionErrorsIdentifyStageWithoutLeakingResponse_data()
    {
        QTest::addColumn<bool>("clientUrlStage");
        QTest::addColumn<QJsonObject>("response");
        QTest::addColumn<QString>("reason");
        QTest::newRow("unsupported-method") << true
            << QJsonObject{{"error", "Connect method not support: must-not-be-displayed"}}
            << QString("站点不支持所选连接方式");
        QTest::newRow("field-validation") << false
            << QJsonObject{{"connect_method", QJsonArray{"must-not-be-displayed"}},
                           {"input_secret", QJsonArray{"must-not-be-displayed"}}}
            << QString("连接方式、输入凭据");
        QTest::newRow("no-publisher-account") << true
            << QJsonObject{{"error", "No host account available, please check the applet, host and account"}}
            << QString("远程应用没有可用的发布机账号");
        QTest::newRow("unrecognized-body") << true
            << QJsonObject{{"error", "must-not-be-displayed"}, {"token", "must-not-be-displayed"}}
            << QString("站点请求失败");
    }
    void connectionErrorsIdentifyStageWithoutLeakingResponse()
    {
        QFETCH(bool, clientUrlStage);
        QFETCH(QJsonObject, response);
        QFETCH(QString, reason);
        HttpMock mock; AppService service; session(service, mock);
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            if (clientUrlStage && r.method == "POST")
                HttpMock::reply(socket, QJsonObject{{"id", "synthetic-connection"}}, 201);
            else
                HttpMock::reply(socket, response, 400);
        };
        QSignalSpy errors(&service, &AppService::error), urls(&service, &AppService::clientUrlReady);
        service.connection({{"asset", "asset"}, {"account", "account"},
                            {"protocol", "http"}, {"connect_method", "chrome"}}, {});
        QTRY_COMPARE(errors.size(), 1);
        const auto error = errors.first().first().toString();
        QVERIFY(error.contains(clientUrlStage ? "获取客户端连接地址失败" : "创建连接令牌失败"));
        QVERIFY(error.contains(reason));
        QVERIFY(error.contains("HTTP 400"));
        QVERIFY(!error.contains("must-not-be-displayed"));
        QCOMPARE(urls.size(), 0);
        QCOMPARE(mock.requests.size(), clientUrlStage ? 2 : 1);
    }
    void staleOrganizationRepliesAreIgnored()
    {
        HttpMock mock; AppService service; session(service, mock);
        QPointer<QTcpSocket> held;
        mock.handler = [&](const HttpRequest &, QTcpSocket *socket) { held = socket; };
        QSignalSpy assets(&service, &AppService::assetsReady);
        service.assets("linux", "", 0, 20);
        QTRY_VERIFY(!held.isNull());
        service.setOrg("new-org");
        HttpMock::reply(held, QJsonObject{{"count", 1}, {"results", QJsonArray{QJsonObject{{"id", "old"}}}}});
        QTest::qWait(100); QCOMPARE(assets.size(), 0);
    }
    void expiredTokenRefreshIsShared()
    {
        HttpMock mock; AppService service; session(service, mock);
        service.m_expires = 1; service.m_clientId = "client+%2B"; service.m_refresh = "refresh+%2B /=&";
        int refreshed = 0;
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            if (r.url.path().endsWith("/token/")) {
                ++refreshed;
                const auto body = decodeForm(r.body);
                QCOMPARE(body.value("grant_type"), QString("refresh_token"));
                QCOMPARE(body.value("client_id"), QString("client+%2B"));
                QCOMPARE(body.value("refresh_token"), QString("refresh+%2B /=&"));
                HttpMock::reply(socket, QJsonObject{{"access_token", "fresh"}, {"expires_in", 3600}});
            } else {
                QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer fresh"));
                HttpMock::reply(socket, QJsonObject{{"id", "asset"}});
            }
        };
        QSignalSpy details(&service, &AppService::detailReady);
        service.detail("a"); service.detail("b");
        QTRY_COMPARE(details.size(), 2); QCOMPARE(refreshed, 1);
    }
    void refreshLifetimeLogsOutAndDiscardsLateReplies()
    {
        HttpMock mock; AppService service;
        service.m_persistCredentials = false;
        QPointer<QTcpSocket> held;
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            const QString path = r.url.path();
            if (path.endsWith("/versions/")) HttpMock::reply(socket, QJsonArray{"4.1.2"});
            else if (path.endsWith("oauth-authorization-server"))
                HttpMock::reply(socket, QJsonObject{{"client_id", "client"}, {"refresh_token_expires_in", 3}});
            else if (path.endsWith("/token/"))
                HttpMock::reply(socket, QJsonObject{{"access_token", "access"}, {"refresh_token", "refresh"}, {"expires_in", 3600}});
            else if (path == "/api/v1/users/profile/") HttpMock::reply(socket, QJsonObject{{"id", "user"}});
            else if (path == "/api/v1/orgs/orgs/current/") HttpMock::reply(socket, QJsonObject{{"id", "org"}});
            else if (path.endsWith("/assets/late/")) held = socket;
            else HttpMock::reply(socket, QJsonObject{});
        };
        QSignalSpy authorization(&service, &AppService::authorizationUrlReady), loggedIn(&service, &AppService::loggedIn);
        QSignalSpy loggedOut(&service, &AppService::loggedOut), details(&service, &AppService::detailReady);
        service.login(mock.url());
        QTRY_COMPARE(authorization.size(), 1);
        const auto state = QUrlQuery(authorization.first().first().toUrl()).queryItemValue("state");
        service.acceptCallback(QUrl("jms://auth/callback?code=test&state=" + state));
        QTRY_COMPARE(loggedIn.size(), 1);
        const qint64 deadline = service.m_refreshExpiresAt;
        QVERIFY(deadline > QDateTime::currentSecsSinceEpoch());
        QVERIFY(service.applyToken(QJsonObject{{"access_token", "rotated"}, {"refresh_token", "rotated-refresh"}, {"expires_in", 3600}}));
        QCOMPARE(service.m_refreshExpiresAt, deadline);
        service.detail("late"); QTRY_VERIFY(!held.isNull());
        const int before = loggedOut.size();
        QTRY_COMPARE_WITH_TIMEOUT(loggedOut.size(), before + 1, 5000);
        QVERIFY(!service.authenticated());
        if (held) HttpMock::reply(held, QJsonObject{{"id", "late"}});
        QTest::qWait(100); QCOMPARE(details.size(), 0);
    }
    void expiredSessionCannotStartRequest()
    {
        HttpMock mock; AppService service; session(service, mock);
        service.m_refreshExpiresAt = QDateTime::currentSecsSinceEpoch() - 1;
        QSignalSpy loggedOut(&service, &AppService::loggedOut);
        service.detail("expired");
        QCOMPARE(loggedOut.size(), 1); QVERIFY(!service.authenticated());
        QTest::qWait(50); QCOMPARE(mock.requests.size(), 0);
    }
    void failedRefreshCompletesAllRequestsAndAllowsRetry()
    {
        HttpMock mock; AppService service; session(service, mock);
        service.m_expires = 1; service.m_clientId = "client"; service.m_refresh = "refresh";
        QPointer<QTcpSocket> held;
        int refreshes = 0;
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            if (r.url.path().endsWith("/token/")) {
                if (++refreshes == 1) held = socket;
                else HttpMock::reply(socket, QJsonObject{{"access_token", "new"}, {"expires_in", 3600}});
            } else HttpMock::reply(socket, QJsonObject{{"id", "new"}});
        };
        QSignalSpy errors(&service, &AppService::error), details(&service, &AppService::detailReady);
        service.detail("a"); service.detail("b"); QTRY_VERIFY(!held.isNull());
        HttpMock::reply(held, QJsonObject{{"error", "invalid_grant"}, {"sensitive", "must-not-be-displayed"}}, 400);
        QTRY_COMPARE(errors.size(), 2); QCOMPARE(details.size(), 0);
        for (const auto &error : errors) QVERIFY(!error.first().toString().contains("must-not-be-displayed"));
        QVERIFY(!service.m_refreshing); QVERIFY(service.m_refreshWaiters.empty());
        service.detail("retry"); QTRY_COMPARE(details.size(), 1); QCOMPARE(refreshes, 2);
    }
    void organizationChangeDropsRefreshWaitersForOldOrganization()
    {
        HttpMock mock; AppService service; session(service, mock);
        service.m_expires = 1; service.m_clientId = "client"; service.m_refresh = "refresh";
        QPointer<QTcpSocket> held;
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            if (r.url.path().endsWith("/token/")) held = socket;
            else {
                QCOMPARE(r.url.path(), QString("/api/v1/perms/users/self/assets/new/"));
                QCOMPARE(r.headers.value("x-jms-org"), QByteArray("new-org"));
                HttpMock::reply(socket, QJsonObject{{"id", "new"}});
            }
        };
        QSignalSpy details(&service, &AppService::detailReady), errors(&service, &AppService::error);
        service.detail("old"); QTRY_VERIFY(!held.isNull());
        service.setOrg("new-org"); service.detail("new");
        HttpMock::reply(held, QJsonObject{{"access_token", "new"}, {"expires_in", 3600}});
        QTRY_COMPARE(details.size(), 1); QCOMPARE(details.first().first().toString(), QString("new"));
        QCOMPARE(mock.requests.size(), 2); QCOMPARE(errors.size(), 0);
    }
    void unauthorizedRetryIsBounded()
    {
        HttpMock mock; AppService service; session(service, mock);
        service.m_clientId = "client"; service.m_refresh = "refresh";
        int refreshes = 0, attempts = 0;
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            if (r.url.path().endsWith("/token/")) {
                ++refreshes;
                HttpMock::reply(socket, QJsonObject{{"access_token", "new"}, {"expires_in", 3600}});
            } else { ++attempts; HttpMock::reply(socket, QJsonObject{}, 401); }
        };
        QSignalSpy errors(&service, &AppService::error);
        service.detail("a"); QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(refreshes, 1); QCOMPARE(attempts, 2);
    }
    void logoutRevokesTheExactRefreshToken()
    {
        HttpMock mock; AppService service; session(service, mock);
        service.m_clientId = "client+%2B"; service.m_refresh = "refresh+%2B /=&";
        mock.handler = [&](const HttpRequest &r, QTcpSocket *socket) {
            QCOMPARE(r.url.path(), QString("/core/auth/oauth2-provider/revoke/"));
            QVERIFY(!r.headers.contains("authorization"));
            const auto body = decodeForm(r.body);
            QCOMPARE(body.value("client_id"), QString("client+%2B"));
            QCOMPARE(body.value("token"), QString("refresh+%2B /=&"));
            QCOMPARE(body.value("token_type_hint"), QString("refresh_token"));
            HttpMock::reply(socket, QJsonObject{});
        };
        service.logout(); QVERIFY(!service.authenticated());
        QTRY_COMPARE(mock.requests.size(), 1);
    }
    void redirectsNeverForwardBearerTokens()
    {
        HttpMock mock, destination; AppService service; session(service, mock);
        mock.handler = [&](const HttpRequest &, QTcpSocket *socket) {
            socket->write("HTTP/1.1 302 Found\r\nLocation: " + destination.url().toUtf8()
                + "/captured\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            socket->disconnectFromHost();
        };
        QSignalSpy errors(&service, &AppService::error);
        service.detail("a"); QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(destination.requests.size(), 0);
    }
    void cancelledLoginCannotConsumeCallback()
    {
        HttpMock mock; AppService service;
        mock.handler = [](const HttpRequest &request, QTcpSocket *socket) {
            if (request.url.path().endsWith("/versions/")) HttpMock::reply(socket, QJsonArray{"4.1.2"});
            else HttpMock::reply(socket, QJsonObject{{"client_id", "client"}});
        };
        QSignalSpy authorization(&service, &AppService::authorizationUrlReady);
        QSignalSpy loginState(&service, &AppService::loginStateChanged);
        service.login(mock.url()); QTRY_COMPARE(authorization.size(), 1);
        QVERIFY(service.loginPending());
        const auto query = QUrlQuery(authorization.first().first().toUrl());
        service.cancelLogin();
        QVERIFY(!service.loginPending());
        QCOMPARE(loginState.size(), 2); QCOMPARE(loginState.last().first().toBool(), false);
        service.acceptCallback(QUrl("jms://auth/callback?code=late&state=" + query.queryItemValue("state")));
        QTest::qWait(50); QCOMPARE(mock.requests.size(), 2); QVERIFY(!service.authenticated());
    }
    void failedAndTimedOutLoginClearsPendingState()
    {
        HttpMock mock; AppService service;
        mock.handler = [](const HttpRequest &, QTcpSocket *socket) { HttpMock::reply(socket, QJsonObject{}, 500); };
        QSignalSpy loginState(&service, &AppService::loginStateChanged), errors(&service, &AppService::error);
        service.login(mock.url()); QVERIFY(service.loginPending());
        QTRY_COMPARE(errors.size(), 1); QVERIFY(!service.loginPending());
        QCOMPARE(loginState.size(), 2);
        QCOMPARE(loginState.first().first().toBool(), true); QCOMPARE(loginState.last().first().toBool(), false);

        QPointer<QTcpSocket> held;
        mock.handler = [&](const HttpRequest &, QTcpSocket *socket) { held = socket; };
        service.login(mock.url()); QVERIFY(service.loginPending()); QTRY_VERIFY(!held.isNull());
        // Exercise the timeout transition without waiting five minutes.
        service.m_loginTimer.start(1);
        QTRY_COMPARE(errors.size(), 2); QVERIFY(!service.loginPending());
        QCOMPARE(loginState.size(), 4); QCOMPARE(loginState.last().first().toBool(), false);
        if (held) HttpMock::reply(held, QJsonObject{{"client_id", "late-client"}});
        QTest::qWait(50); QVERIFY(!service.loginPending()); QCOMPARE(loginState.size(), 4);
    }
    void restoreWithoutCredentialsEndsPendingState()
    {
        HttpMock mock; AppService service;
        service.m_persistCredentials = false;
        QSignalSpy loginState(&service, &AppService::loginStateChanged);
        service.restore(mock.url());
        QVERIFY(!service.loginPending()); QVERIFY(!service.authenticated());
        QVERIFY(!service.m_loginTimer.isActive());
        QCOMPARE(loginState.size(), 2);
        QCOMPARE(loginState.first().first().toBool(), true); QCOMPARE(loginState.last().first().toBool(), false);
        QCOMPARE(mock.requests.size(), 0);
    }
    void loopbackCallbackRejectsInvalidState()
    {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 14876)) QSKIP("OAuth loopback port is in use");
        probe.close();
        HttpMock mock; AppService service;
        service.m_persistCredentials = false;
        mock.handler = [](const HttpRequest &request, QTcpSocket *socket) {
            if (request.url.path().endsWith("/versions/")) HttpMock::reply(socket, QJsonArray{"4.1.2"});
            else HttpMock::reply(socket, QJsonObject{{"client_id", "client"}});
        };
        QSignalSpy authorization(&service, &AppService::authorizationUrlReady);
        service.login(mock.url(), true); QTRY_COMPARE(authorization.size(), 1);
        QVERIFY(service.m_callback.isListening());
        QNetworkAccessManager network;
        auto *reply = network.get(QNetworkRequest(QUrl("http://127.0.0.1:14876/auth/callback?code=bad&state=wrong")));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 400);
        reply->deleteLater();
        QVERIFY(service.m_callback.isListening());
        QCOMPARE(mock.requests.size(), 2);
        service.cancelLogin(); QVERIFY(!service.m_callback.isListening());
        QVERIFY(probe.listen(QHostAddress::LocalHost, 14876));
    }
    void proxyConfigurationSnapshot() {
        AppService service;
        QSettings settings;
        settings.remove("proxy/mode"); settings.remove("proxy/http");
        settings.remove("proxy/username");
        service.applyProxy();
        QCOMPARE(service.m_proxyMode, QString("system"));
        settings.setValue("proxy/mode", "");
        service.applyProxy();
        QCOMPARE(service.m_proxyMode, QString("system"));
        settings.setValue("proxy/mode", "manual");
        settings.setValue("proxy/http", "http://proxy.example.com:8080");
        service.applyProxy();
        QCOMPARE(service.m_proxyMode, QString("manual"));
        QCOMPARE(service.m_proxyAddress, QString("http://proxy.example.com:8080"));
        settings.setValue("proxy/http", "proxy.example.com");
        service.applyProxy();
        QCOMPARE(service.m_proxyAddress, QString("proxy.example.com"));
        settings.setValue("proxy/mode", "none");
        service.applyProxy();
        QCOMPARE(service.m_proxyMode, QString("none"));
        settings.remove("proxy/mode"); settings.remove("proxy/http");
        settings.remove("proxy/username");
    }
};

QTEST_MAIN(TestAppService)
#include "test_appservice.moc"
