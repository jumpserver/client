#include "platform.h"
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>

class PlatformTests : public QObject {
    Q_OBJECT
private slots:
    void credentialStoreRoundTrip() {
        if (qEnvironmentVariable("CLIENT_QT_TEST_CREDENTIAL_STORE") != QStringLiteral("1"))
            QSKIP("Set CLIENT_QT_TEST_CREDENTIAL_STORE=1 to test a unique synthetic system credential");
        const QString key = QStringLiteral("client-qt-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        struct Cleanup {
            QString key;
            ~Cleanup() { CredentialStore::remove(key); }
        } cleanup{key};
        QString error;
        const QByteArray value("synthetic-test-value-not-a-real-secret");
        QVERIFY2(CredentialStore::write(key, value, &error), qPrintable(error));
        const QByteArray loaded = CredentialStore::read(key, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(loaded, value);
        const QByteArray updated("updated-synthetic-value-not-a-real-secret");
        QVERIFY2(CredentialStore::write(key, updated, &error), qPrintable(error));
        QCOMPARE(CredentialStore::read(key, &error), updated);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(CredentialStore::remove(key, &error), qPrintable(error));
        QVERIFY(CredentialStore::read(key, &error).isEmpty());
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    void mergePreservesSelectedPrograms() {
        QJsonObject oldItem{{"name", "terminal"}, {"path", "/custom/path"}, {"is_set", true},
                            {"match_first", QJsonArray{"ssh"}}};
        QJsonObject newItem{{"name", "terminal"}, {"path", "/default/path"}, {"is_set", false},
                            {"arg_format", "updated"}, {"match_first", QJsonArray{}}};
        QJsonObject user{{"version", 1}, {"linux", QJsonObject{{"terminal", QJsonArray{oldItem}}}}};
        QJsonObject defaults{{"version", 2}, {"linux", QJsonObject{{"terminal", QJsonArray{newItem}}}}};
        auto merged = ConfigStore::merge(user, defaults);
        auto item = merged.value("linux").toObject().value("terminal").toArray().first().toObject();
        QCOMPARE(merged.value("version").toInt(), 2);
        QCOMPARE(item.value("path").toString(), QString("/custom/path"));
        QCOMPARE(item.value("arg_format").toString(), QString("updated"));
        QVERIFY(item.value("is_set").toBool());
        QCOMPARE(item.value("match_first").toArray(), QJsonArray{"ssh"});
    }
    void configurationIsAtomicAndPreserved() {
        QTemporaryDir dir;
        const QString path = dir.filePath("config.json");
        ConfigStore store(nullptr, path);
        const QJsonObject apps = store.applications();
        QVERIFY2(!apps.isEmpty(), qPrintable(store.error()));
        const auto terminals = apps.value("terminal").toArray();
        QVERIFY(!terminals.isEmpty());
        const auto selected = terminals.first().toObject();
        const QString name = selected.value("name").toString();
        const auto protocols = selected.value("protocol").toArray();
        QVERIFY(!protocols.isEmpty());
        const QString protocol = protocols.first().toString();
        QVERIFY(store.selectApplication("terminal", protocol, name, "/custom/path"));
        QVERIFY(store.selectApplication("terminal", protocol, name));
        const auto after = store.applications().value("terminal").toArray().first().toObject();
        QCOMPARE(after.value("path").toString(), QString("/custom/path"));
        QVERIFY(after.value("match_first").toArray().contains(protocol));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray before = file.readAll(); file.close();
        QVERIFY(!store.selectApplication("terminal", protocol, "missing"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), before); file.close();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("broken json"); file.close();
        QVERIFY(store.applications().isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("broken json"));
    }
    void clearDefaultApplicationOnlyTouchesTheChosenProtocol() {
        QTemporaryDir dir;
        ConfigStore store(nullptr, dir.filePath("config.json"));
        const auto first = store.applications().value("terminal").toArray().first().toObject();
        const QString name = first.value("name").toString();
        const QJsonArray protocols = first.value("protocol").toArray();
        QVERIFY(protocols.size() >= 2);
        const QString kept = protocols.at(0).toString(), cleared = protocols.at(1).toString();
        QVERIFY(store.selectApplication("terminal", kept, name));
        QVERIFY(store.selectApplication("terminal", cleared, name));
        QFile file(dir.filePath("config.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray before = file.readAll(); file.close();
        // Clearing a protocol nobody owns keeps the file untouched, and an empty
        // protocol is reported instead of silently rewriting every entry.
        QVERIFY(store.clearDefaultApplication("terminal", "sftp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), before);
        file.close();
        QVERIFY(!store.clearDefaultApplication("terminal", QString()));
        QVERIFY(!store.error().isEmpty());
        QVERIFY(store.clearDefaultApplication("terminal", cleared));
        const auto after = store.applications().value("terminal").toArray().first().toObject();
        QVERIFY(after.value("match_first").toArray().contains(kept));
        QVERIFY(!after.value("match_first").toArray().contains(cleared));
    }
    void connectionAddressValidation() {
        QVERIFY(!Launcher::isConnectionUrl("jms://auth/callback?code=secret"));
        QVERIFY(!Launcher::isConnectionUrl("https://example.com"));
        QVERIFY(!Launcher::isConnectionUrl("jms://!!!!"));
        QVERIFY(!Launcher::isConnectionUrl("jms://e30="));
        QVERIFY(Launcher::isConnectionUrl("jms://" + QString::fromLatin1(QByteArray("{\"protocol\":\"ssh\"}").toBase64())));
    }
    void connectionPayloadMustStayRaw() {
        // JMS places case-sensitive base64 in the authority, which QUrl normalizes.
        for (const QByteArray payload : {QByteArray("{\"protocol\":\"ssh\"}"),
             QByteArray("{\"protocol\":\"ssh\",\"host\":\"127.0.0.1\",\"port\":22,\"username\":\"synthetic\"}")}) {
            const QString raw = "jms://" + QString::fromLatin1(payload.toBase64());
            QVERIFY(Launcher::isConnectionUrl(raw));
            const QString normalized = QUrl(raw).toString(QUrl::FullyEncoded);
            QVERIFY(normalized != raw);
            QVERIFY(!Launcher::isConnectionUrl(normalized));
        }
    }
    void forwardsToFirstInstance() {
        const QString name = "cqt-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        SingleInstance first(nullptr, name);
        QVERIFY2(first.start({"first"}), qPrintable(first.error()));
        QSignalSpy messages(&first, &SingleInstance::message);
        SingleInstance second(nullptr, name);
        QVERIFY(!second.start({"second", "jms://test"}));
        QVERIFY(second.error().isEmpty());
        QTRY_COMPARE(messages.count(), 1);
        QCOMPARE(messages.takeFirst().at(0).toStringList(), QStringList({"second", "jms://test"}));
    }
#ifndef Q_OS_WIN
    void helperErrorsAreReportedEvenWithZeroExit_data() {
        QTest::addColumn<QByteArray>("payload");
        QTest::newRow("ssh") << QByteArray("{\"protocol\":\"ssh\",\"host\":\"127.0.0.1\"}");
        QTest::newRow("http-applet-rdp") << QByteArray("{\"protocol\":\"rdp\",\"file\":{\"name\":\"synthetic.rdp\",\"content\":\"remoteapplicationmode:i:1\\r\\n\"}}");
    }
    void helperErrorsAreReportedEvenWithZeroExit() {
        QFETCH(QByteArray, payload);
        QTemporaryDir dir;
        const QString path = dir.filePath("helper");
        QFile helper(path);
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("#!/bin/sh\nprintf '%s' \"$1\" > captured-argument\nprintf 'Error: test-only secret should not leak\\n' >&2\nexit 0\n");
        helper.close();
        QVERIFY(helper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        Launcher launcher(nullptr, path, dir.filePath("config.json"));
        QSignalSpy errors(&launcher, &Launcher::failed), ended(&launcher, &Launcher::finished);
        const QString raw = "jms://" + QString::fromLatin1(payload.toBase64());
        launcher.launch(raw);
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(errors.count(), 1);
        QVERIFY(!errors.first().first().toString().contains("secret"));
        QFile captured(dir.filePath("captured-argument"));
        QVERIFY(captured.open(QIODevice::ReadOnly));
        QCOMPARE(captured.readAll(), raw.toUtf8());
    }
#endif
};
QTEST_GUILESS_MAIN(PlatformTests)
#include "test_platform.moc"
