#pragma once

#include "../bridge/frame_bridge.h"
#include "renderer.h"

#include <QElapsedTimer>
#include <QProcess>

#include <functional>

// Parent-side adapter.  The implementation process is the same executable in
// --renderer-child mode, so a libmpv or Chromium failure cannot terminate the
// daemon that owns the Unix JSON-RPC socket.
class IsolatedRenderer final : public Renderer {
  Q_OBJECT

 public:
  explicit IsolatedRenderer(RendererSpec spec, QObject *parent = nullptr);
  ~IsolatedRenderer() override;

  bool start(QString *error) override;
  void stop() override;
  void pause() override;
  void resume() override;
  QImage lastFrame() const override;
  QString rendererName() const override;
  bool isRunning() const override;
  bool isFallback() const override;
  qint64 processId() const override;
  double frameRate() const override;

  // Set by the owner before start().  The view is mapped child memory and is
  // valid only until this synchronous callback returns.
  using SceneTransportSink = std::function<SceneTransportPublishResult(
      const SceneTransportView &, quint64 *)>;
  using BridgeSnapshot = std::function<QImage()>;
  void setSceneTransportCallbacks(SceneTransportSink sink, BridgeSnapshot snapshot);
#ifdef ANISPAPER_ISOLATED_RENDERER_TESTING
  bool openSceneTransportForTesting(const QString &name) { return openSceneTransport(name); }
  bool hasSceneTransportForTesting() const {
    return sceneTransportFd_ >= 0 || sceneTransportMap_ != nullptr;
  }
#endif

 private:
  void sendCommand(const QString &command);
  void readFrames();
  void parseLine(const QByteArray &line);
  void reportFatal(const QString &reason);
  void cleanupProcessGroup();
  bool openSceneTransport(const QString &name);
  void closeSceneTransport();
  SceneTransportPublishResult publishSceneTransportFrame();
  // Copies the newest published transport frame into frame_; false when no
  // new frame since the last call (or the transport is unavailable).
  bool copySceneTransportFrame();

  // Binary scene transport (scene-engine children only): the child announces
  // exactly one POSIX shm object with a 64-byte header + N RGBA8888 slots;
  // "frame" events without a "jpeg" payload are plain notifications and the
  // pixel bytes are copied once from the mapped transport into frame_.
  int sceneTransportFd_ = -1;
  void *sceneTransportMap_ = nullptr;
  size_t sceneTransportMapSize_ = 0;
  quint64 sceneTransportLastSeq_ = 0;
  SceneTransportView sceneTransportView_;
  SceneTransportSink sceneTransportSink_;
  BridgeSnapshot bridgeSnapshot_;

  QProcess process_;
  QByteArray input_;
  QImage frame_;
  QElapsedTimer fpsClock_;
  int frameCount_ = 0;
  double fps_ = 0.0;
  bool running_ = false;
  bool stopRequested_ = false;
  bool fallback_ = false;
  bool fatalReported_ = false;
  QString childFailure_;
  QString sceneEnginePath_;
  qint64 childPid_ = 0;
};
