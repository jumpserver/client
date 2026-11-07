#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QSet>
#include <QUrl>
class AppService;
class ConfigStore;
class Launcher;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTimer;
class QCheckBox;
class QMenu;
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  MainWindow(AppService *, ConfigStore *, Launcher *,
             QWidget *parent = nullptr);
  void handleUrl(const QString &rawUrl);
public slots:
  void showSettings();
signals:
  void preferencesChanged();

protected:
  void closeEvent(QCloseEvent *) override;
  void changeEvent(QEvent *) override;

private:
  void buildUi();
  void retranslateUi();
  void login();
  void refresh(bool reset = false);
  void renderAssets();
  void setFavorite(const QString &id, bool enabled);
  void showContextMenu(const QPoint &, bool grid);
  void loadProtocolMenu(QMenu *menu, const QJsonObject &asset);
  void requestConnection(const QJsonObject &, bool editOnly = false,
                         const QString &protocol = QString());
  void consumeDetails(const QString &, const QJsonObject &);
  void processNextConnection();
  void updateSessionUi();
  void updateLoginState();
  void updateSelectionUi();
  void switchSite(const QString &);
  void showError(const QString &, bool statusOnly = false);
  QJsonObject selectedAsset(bool grid) const;
  QString scope() const;
  QList<QJsonObject> selectedAssets() const;
  AppService *m_service;
  ConfigStore *m_config;
  Launcher *m_launcher;
  QListWidget *m_categories, *m_grid;
  QTableWidget *m_table;
  QLineEdit *m_search;
  QComboBox *m_sites, *m_organizations, *m_order, *m_pageSize;
  QStackedWidget *m_content, *m_views;
  QWidget *m_authTools;
  QWidget *m_restorePage;
  QLabel *m_restoreTitle;
  QPushButton *m_cancelRestore;
  QLabel *m_profile, *m_pageLabel, *m_empty, *m_loginMessage,
      *m_welcome, *m_catalogLabel, *m_loginSubtitle, *m_siteLabel, *m_loginHint;
  QPushButton *m_loginButton, *m_cancelLogin, *m_logout, *m_settingsButton,
      *m_connectButton, *m_refreshButton, *m_layoutButton, *m_previous, *m_next,
      *m_collapse, *m_switcher, *m_reopen, *m_copyLogin, *m_sortButton;
  QTimer *m_searchTimer;
  QJsonArray m_assets;
  QJsonObject m_methods, m_profileData, m_publicSettings;
  QElapsedTimer m_methodsAge;
  quint64 m_methodsCacheRevision = 0;
  QJsonValue m_version;
  QSet<QString> m_favorites;
  QSet<QString> m_pendingFavorites;
  QSet<QString> m_visibleErrors;
  QList<QJsonObject> m_connectionQueue;
  QJsonObject m_pendingAsset;
  QJsonObject m_waitingDetail;
  QString m_pendingProtocol;
  QUrl m_authorizationUrl;
  bool m_editOnly = false, m_methodsLoaded = false, m_loading = false,
       m_connectionInFlight = false, m_collapsed = false, m_restoring = false,
       m_forceConnectionDialog = false;
  int m_offset = 0, m_count = 0;
};
