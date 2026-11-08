#pragma once

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QDeadlineTimer>
#include <functional>

struct NetworkResult {
    QByteArray body;
    int status = 0;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    QString failure;
};

class NetworkWorker : public QObject {
public:
    using Completion = std::function<void(NetworkResult)>;
    explicit NetworkWorker(QObject *parent = nullptr);
    void send(QNetworkRequest request, QByteArray method, QByteArray body,
              QString mode, QString address, quint64 generation, Completion completion);
    void cancelBefore(quint64 generation);
private:
    void sendResolved(QNetworkRequest request, QByteArray method, QByteArray body,
                      QList<QNetworkProxy> proxies, quint64 generation,
                      Completion completion, QDeadlineTimer deadline);
    quint64 m_generation = 0;
};
