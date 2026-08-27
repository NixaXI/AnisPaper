#pragma once

#include "catalog_model.h"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <functional>

class QDialog;
class QLabel;
class QLineEdit;
class QProcess;
class QPushButton;

class RpcClient : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool online READ online NOTIFY onlineChanged)
  Q_PROPERTY(bool applying READ applying NOTIFY applyingChanged)
  Q_PROPERTY(QString statusLine READ statusLine NOTIFY statusLineChanged)
  Q_PROPERTY(QString toast READ toast NOTIFY toastChanged)
  Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
  Q_PROPERTY(QString selectedId READ selectedId WRITE setSelectedId NOTIFY selectedIdChanged)
  Q_PROPERTY(QString selectedOutput READ selectedOutput WRITE setSelectedOutput NOTIFY
                 selectedOutputChanged)
  Q_PROPERTY(QVariantList monitors READ monitors NOTIFY monitorsChanged)
  Q_PROPERTY(QStringList monitorNames READ monitorNames NOTIFY monitorsChanged)
  Q_PROPERTY(CatalogModel *catalog READ catalog CONSTANT)
  Q_PROPERTY(int volumePercent READ volumePercent WRITE setVolumePercent NOTIFY volumePercentChanged)
  Q_PROPERTY(int fpsCap READ fpsCap WRITE setFpsCap NOTIFY fpsCapChanged)
  Q_PROPERTY(QString measuredFps READ measuredFps NOTIFY measuredFpsChanged)
  Q_PROPERTY(QVariantList liveRenderers READ liveRenderers NOTIFY liveRenderersChanged)
  Q_PROPERTY(bool gamingActive READ gamingActive NOTIFY gamingActiveChanged)
  Q_PROPERTY(bool workshopBusy READ workshopBusy NOTIFY workshopBusyChanged)
  Q_PROPERTY(QString workshopError READ workshopError NOTIFY workshopErrorChanged)
  Q_PROPERTY(QVariantList workshopItems READ workshopItems NOTIFY workshopItemsChanged)
  Q_PROPERTY(int workshopPage READ workshopPage NOTIFY workshopPageChanged)
  Q_PROPERTY(int workshopPageCount READ workshopPageCount NOTIFY workshopPageChanged)
  Q_PROPERTY(bool downloadActive READ downloadActive NOTIFY downloadChanged)
  Q_PROPERTY(int downloadPercent READ downloadPercent NOTIFY downloadChanged)
  Q_PROPERTY(QString downloadLabel READ downloadLabel NOTIFY downloadChanged)
  Q_PROPERTY(bool steamAuthNeeded READ steamAuthNeeded NOTIFY steamAuthNeededChanged)
  Q_PROPERTY(bool steamGuardNeeded READ steamGuardNeeded NOTIFY steamAuthNeededChanged)
  Q_PROPERTY(QString steamAuthUser READ steamAuthUser NOTIFY steamAuthNeededChanged)
  Q_PROPERTY(QString steamAuthQr READ steamAuthQr NOTIFY steamAuthNeededChanged)
  Q_PROPERTY(QString steamAuthQrUrl READ steamAuthQrUrl NOTIFY steamAuthNeededChanged)
  Q_PROPERTY(QString steamAuthQrLink READ steamAuthQrLink NOTIFY steamAuthNeededChanged)
  Q_PROPERTY(bool steamUseClient READ steamUseClient NOTIFY steamAuthNeededChanged)

 public:
  explicit RpcClient(QObject *parent = nullptr);

  bool online() const { return online_; }
  bool applying() const { return applying_; }
  QString statusLine() const { return statusLine_; }
  QString toast() const { return toast_; }
  QString filter() const { return filter_; }
  QString selectedId() const { return selectedId_; }
  QString selectedOutput() const { return selectedOutput_; }
  QVariantList monitors() const { return monitors_; }
  QStringList monitorNames() const { return monitorNames_; }
  CatalogModel *catalog() { return &catalog_; }
  int volumePercent() const { return volumePercent_; }
  int fpsCap() const { return fpsCap_; }
  QString measuredFps() const { return measuredFps_; }
  QVariantList liveRenderers() const { return liveRenderers_; }
  bool gamingActive() const { return gamingActive_; }
  bool workshopBusy() const { return workshopBusy_; }
  QString workshopError() const { return workshopError_; }
  QVariantList workshopItems() const { return workshopItems_; }
  int workshopPage() const { return workshopPage_; }
  int workshopPageCount() const { return workshopPageCount_; }
  bool downloadActive() const { return downloadActive_; }
  int downloadPercent() const { return downloadPercent_; }
  QString downloadLabel() const { return downloadLabel_; }
  bool steamAuthNeeded() const { return steamAuthNeeded_; }
  bool steamGuardNeeded() const { return steamGuardNeeded_; }
  QString steamAuthUser() const { return steamAuthUser_; }
  QString steamAuthQr() const { return steamAuthQr_; }
  QString steamAuthQrUrl() const { return steamAuthQrUrl_; }
  QString steamAuthQrLink() const { return steamAuthQrLink_; }
  bool steamUseClient() const { return steamUseClient_; }

  void setFilter(const QString &filter);
  void setSelectedId(const QString &id);
  void setSelectedOutput(const QString &output);
  void setVolumePercent(int percent);
  void setFpsCap(int fps);

  Q_INVOKABLE void applySelected();
  Q_INVOKABLE void stopSelected();
  Q_INVOKABLE void refreshCatalog();
  Q_INVOKABLE void dismissToast();
  Q_INVOKABLE QVariantList catalogItems() const;
  Q_INVOKABLE void applyWallpaper(const QString &id, const QString &output);
  Q_INVOKABLE void stopWallpaper(const QString &output);
  Q_INVOKABLE void setGamingMode(const QString &mode);
  Q_INVOKABLE void openExternal(const QString &url);
  Q_INVOKABLE void searchWorkshop(const QString &query, int page, const QString &ratings);
  Q_INVOKABLE void subscribeWorkshop(const QString &publishedFileId);
  Q_INVOKABLE void submitSteamAuth(const QString &user, const QString &password,
                                   const QString &guard);
  Q_INVOKABLE void cancelSteamAuth();
  Q_INVOKABLE void startSteamQr();
  Q_INVOKABLE void openSteamForWorkshop();

 signals:
  void onlineChanged();
  void applyingChanged();
  void statusLineChanged();
  void toastChanged();
  void filterChanged();
  void selectedIdChanged();
  void selectedOutputChanged();
  void monitorsChanged();
  void volumePercentChanged();
  void fpsCapChanged();
  void measuredFpsChanged();
  void liveRenderersChanged();
  void gamingActiveChanged();
  void catalogChanged();
  void workshopBusyChanged();
  void workshopErrorChanged();
  void workshopItemsChanged();
  void workshopPageChanged();
  void downloadChanged();
  void steamAuthNeededChanged();

 private:
  using Reply = std::function<void(const QJsonValue &result, const QString &error)>;

  void connectDaemon();
  void disconnectDaemon(const QString &reason);
  void send(const QString &method, const QJsonObject &params, Reply reply);
  void flushLines();
  void handleObject(const QJsonObject &object);
  void bootstrap();
  void setOnline(bool online, const QString &line);
  void setToast(const QString &message);
  void setStatusLine(const QString &line);
  void setApplying(bool applying);
  void pushSettings();
  void pollStatus();
  void setWorkshopBusy(bool busy);
  void setWorkshopError(const QString &error);
  void fetchWorkshopDetails(const QStringList &ids, int gen);
  void ensureSteamcmdThenInstall(const QString &id);
  void runSteamcmdInstall(const QString &id);
  void runLicensedSteamcmd(const QString &id);
  void runDepotDownloader(const QString &id);
  void requestSteamAuth(const QString &id);
  void startQrDownload(const QString &id);
  void stopQrProcess();
  void startUgcDownload(const QString &id);
  void stopUgcProcess();
  void finishWorkshopInstall(const QString &id, bool ok, const QString &detail);
  void setDownload(bool active, int percent, const QString &label);
  void setSteamAuthNeeded(bool needed, bool guard);
  void syncNativeSteamAuth();
  void pollDownloadProgress();
  void startSteamClientWait(const QString &id, const QString &steamcmdError);
  void resumePendingWorkshop();
  void fetchDownloadSize(const QString &id);
  qint64 expectedDownloadBytes(const QString &id) const;
  void wipeSteamSecrets();

  CatalogModel catalog_;
  QLocalSocket socket_;
  QTimer reconnect_;
  QTimer settingsDebounce_;
  QTimer statusPoll_;
  QByteArray buffer_;
  QHash<qint64, Reply> pending_;
  qint64 nextId_ = 1;
  bool online_ = false;
  bool applying_ = false;
  bool bootstrapping_ = false;
  QString statusLine_ = QStringLiteral("Buscando anis-paperd…");
  QString toast_;
  QString filter_;
  QString selectedId_;
  QString selectedOutput_;
  QVariantList monitors_;
  QStringList monitorNames_;
  int volumePercent_ = 0;
  int fpsCap_ = 60;
  QString measuredFps_ = QStringLiteral("— fps");
  QVariantList liveRenderers_;
  bool gamingActive_ = false;
  QNetworkAccessManager http_;
  bool workshopBusy_ = false;
  QString workshopError_;
  QVariantList workshopItems_;
  int workshopPage_ = 1;
  int workshopPageCount_ = 1;
  int workshopSearchGen_ = 0;
  QPointer<QNetworkReply> workshopBrowseReply_;
  QPointer<QNetworkReply> workshopDetailsReply_;
  bool workshopInstallBusy_ = false;
  QSet<QString> workshopRatings_{QStringLiteral("everyone"), QStringLiteral("questionable")};
  bool downloadActive_ = false;
  int downloadPercent_ = 0;
  QString downloadLabel_;
  QString downloadId_;
  qint64 downloadExpectedBytes_ = 0;
  int downloadAttempt_ = 0;
  qint64 downloadWaitUntilMs_ = 0;
  qint64 downloadLastBytes_ = 0;
  qint64 downloadBeganMs_ = 0;
  QString downloadError_;
  QByteArray downloadLog_;
  QTimer downloadPoll_;
  bool steamAuthNeeded_ = false;
  bool steamGuardNeeded_ = false;
  QString steamAuthUser_;
  QString steamAuthPassword_;
  QString steamAuthGuard_;
  bool licensedAttempted_ = false;
  bool depotAttempted_ = false;
  int qrRestarts_ = 0;
  QString steamAuthQr_;
  QString steamAuthQrUrl_;
  QString steamAuthQrLink_;
  QPointer<QProcess> qrProcess_;
  QPointer<QProcess> ugcProcess_;
  QPointer<QDialog> steamAuthDialog_;
  QPointer<QLabel> steamAuthQrLabel_;
  QPointer<QLineEdit> steamAuthUserEdit_;
  QPointer<QLineEdit> steamAuthPassEdit_;
  QPointer<QLineEdit> steamAuthGuardEdit_;
  QPointer<QPushButton> steamAuthQrOpenBtn_;
  QPointer<QLabel> steamAuthHintLabel_;
  QPointer<QPushButton> steamAuthGoBtn_;
  bool qrPromptOpened_ = false;
  QString lastOpenedQrLink_;
  bool qrWanted_ = false;
  bool steamUseClient_ = false;
  qint64 lastSteamPokeMs_ = 0;
};
