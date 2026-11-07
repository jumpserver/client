#pragma once
#include <QDialog>
#include <QJsonObject>
class QComboBox;
class QLineEdit;
class QCheckBox;
class QLabel;
class QDialogButtonBox;
class QScrollArea;
class QTabWidget;
class ConnectionDialog : public QDialog {
  Q_OBJECT
public:
  ConnectionDialog(const QJsonObject &asset, const QJsonObject &details,
                   const QJsonObject &methods, const QString &scope,
                   bool rememberAllowed, bool editOnly,
                   const QString &preferredProtocol, QWidget *parent = nullptr);
  QJsonObject requestBody() const;
  QJsonObject extraParameters() const;
  bool canConnectDirectly(bool explicitProtocol = false) const;
  bool tryDirectConnection(bool explicitProtocol = false);
  static QString connectionKey(const QString &scope, const QString &assetId);
  static QJsonObject saved(const QString &scope, const QString &assetId);
  static QString displayAddress(const QJsonObject &asset, const QJsonObject &saved);
  static bool isClientMethod(const QString &protocol, const QJsonObject &method);

protected:
  void showEvent(QShowEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void changeEvent(QEvent *event) override;

private:
  void updateMethods();
  void updateMethodPage();
  void updateAccount();
  void updateValidation();
  void saveAndAccept();
  void scheduleFitHeight();
  void fitHeightToContents();
  QJsonObject m_asset, m_details, m_methods, m_saved, m_selectedMethod;
  QString m_key, m_requestedProtocol;
  bool m_rememberAllowed;
  bool m_usernameRequired = false;
  bool m_fitQueued = false;
  bool m_initialPlacement = true;
  QScrollArea *m_scroll = nullptr;
  QWidget *m_footer = nullptr;
  QComboBox *m_protocol, *m_account;
  QTabWidget *m_methodTabs;
  QLineEdit *m_username, *m_secret;
  QCheckBox *m_remember;
  QWidget *m_usernameField, *m_secretField, *m_methodField;
  QLabel *m_usernameLabel, *m_secretLabel, *m_message;
  QDialogButtonBox *m_buttons;
};
