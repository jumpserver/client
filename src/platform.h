#pragma once

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QStringList>
#include <QUrl>
#include <memory>

class QLocalServer;
class QLockFile;

// No on-disk plaintext fallback. An unavailable OS credential service is an error.
class CredentialStore {
public:
    static QByteArray read(const QString &key, QString *error = nullptr);
    static bool write(const QString &key, const QByteArray &value, QString *error = nullptr);
    static bool remove(const QString &key, QString *error = nullptr);
};

class SystemProxy {
public:
    static QList<QNetworkProxy> resolve(const QUrl &url, QString *failure = nullptr);
    // Matches a host against the system exclusion syntax ("*.corp", "10.*",
    // "192.168.0.0/16", "intranet"). Kept platform-independent for testing.
    static bool hostExcluded(const QString &host, const QStringList &patterns,
                             bool excludeSimpleHostnames = false);
};

class ConfigStore : public QObject {
    Q_OBJECT
public:
    explicit ConfigStore(QObject *parent = nullptr, const QString &path = QString());
    QJsonObject applications();
    bool selectApplication(const QString &category, const QString &protocol,
                           const QString &name, const QString &path = QString());
    // Remove the default client for a protocol. Connections then fail until the
    // user selects one again, matching the local application switch semantics.
    bool clearDefaultApplication(const QString &category, const QString &protocol);
    QString error() const { return error_; }
    static QString configPath();
    static QString platformKey();
    static QJsonObject merge(const QJsonObject &user, const QJsonObject &defaults);
signals:
    void changed();
private:
    bool load(QJsonObject &config);
    bool save(const QJsonObject &config);
    QString path_, error_;
};

class Launcher : public QObject {
    Q_OBJECT
public:
    explicit Launcher(QObject *parent = nullptr, const QString &executable = QString(),
                      const QString &config = QString());
    void launch(const QString &url);
    static QString executablePath();
    static bool isConnectionUrl(const QString &url);
signals:
    void started();
    void failed(const QString &message);
    void finished();
private:
    QString executable_, config_;
};

class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject *parent = nullptr, const QString &name = QString());
    ~SingleInstance() override;
    // false means another process accepted the arguments, or error() is nonempty.
    bool start(const QStringList &args);
    QString error() const { return error_; }
signals:
    void message(const QStringList &args);
private:
    QString name_, error_;
    QLocalServer *server_ = nullptr;
    std::unique_ptr<QLockFile> lock_;
};
