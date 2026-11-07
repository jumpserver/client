#pragma once
#include <QDialog>
#include <QJsonValue>
class ConfigStore;
class QTableWidget;
class QComboBox;
class QLabel;
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
  void reloadApplications();
  bool m_initialPlacement = true;
  ConfigStore *m_config;
  QComboBox *m_protocol;
  QTableWidget *m_applications;
  QLabel *m_applicationError;
};
