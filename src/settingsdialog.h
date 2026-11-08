#pragma once
#include <QDialog>
#include <QJsonValue>
#include <functional>
class ConfigStore;
class QButtonGroup;
class QJsonObject;
class QLabel;
class QVBoxLayout;
class QWidget;
class SettingsDialog : public QDialog {
  Q_OBJECT
public:
  explicit SettingsDialog(ConfigStore *config, const QJsonValue &version,
                          QWidget *parent = nullptr);
signals:
  void preferencesChanged();

protected:
  void showEvent(QShowEvent *event) override;

private:
  void retranslateUi();
  QList<std::function<void()>> m_translations;
  QString selectedProtocol() const;
  QWidget *applicationCard(const QJsonObject &item, const QString &category,
                           const QString &protocol);
  void reloadApplications();
  bool m_initialPlacement = true;
  ConfigStore *m_config;
  QButtonGroup *m_protocolGroup = nullptr;
  QVBoxLayout *m_applicationCards = nullptr;
  QLabel *m_applicationError;
};
