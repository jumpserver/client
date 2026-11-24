#pragma once

#include "ffmpegplugin.h"
#include <QThread>

namespace Replay {

// Off-thread I/O, decoding, and child process waits. Ownership and all UI
// callbacks stay on the GUI thread.
class Task : public QObject {
public:
  using Work = std::function<void(const Canceled &, const Progress &)>;
  using Done = std::function<void(const QString &, bool)>;
  explicit Task(QObject *parent) : QObject(parent) {}
  ~Task() override {
    cancel();
    if (m_thread) m_thread->wait();
  }
  bool busy() const { return m_thread != nullptr; }
  void cancel() { if (m_thread) m_thread->requestInterruption(); }
  void start(Work work, Done done, Progress progress = {}) {
    Q_ASSERT(!m_thread);
    auto error = std::make_shared<QString>();
    auto canceled = std::make_shared<bool>(false);
    m_thread = QThread::create([this, work, error, canceled, progress] {
      const auto stop = [] { return QThread::currentThread()->isInterruptionRequested(); };
      int reported = -2;
      try {
        work(stop, [&](int percent) {
          if (!progress || reported == percent) return;
          reported = percent;
          QMetaObject::invokeMethod(this, [progress, percent] { progress(percent); }, Qt::QueuedConnection);
        });
      } catch (const Error &failure) {
        *canceled = stop();
        if (!*canceled) *error = failure.message;
      } catch (const std::bad_alloc &) {
        *error = QStringLiteral("There is not enough memory to complete the replay task.");
      }
    });
    m_thread->setParent(this);
    connect(m_thread, &QThread::finished, this, [this, error, canceled, done] {
      m_thread->wait();
      m_thread->deleteLater();
      m_thread = nullptr;
      done(*error, *canceled);
    });
    m_thread->start();
  }

private:
  QThread *m_thread = nullptr;
};

} // namespace Replay
