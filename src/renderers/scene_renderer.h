#pragma once

#include "renderer.h"

#include <memory>

class StaticImageRenderer;

// F4's intentionally fail-closed scene implementation.  F0 found no
// libwallpaperengine on this host, so scenes are represented by their catalog
// preview rather than attempting an unverified native renderer in plasma.
class SceneRenderer final : public Renderer {
  Q_OBJECT

 public:
  explicit SceneRenderer(RendererSpec spec, QObject *parent = nullptr);
  ~SceneRenderer() override;

  bool start(QString *error) override;
  void stop() override;
  void pause() override;
  void resume() override;
  QImage lastFrame() const override;
  QString rendererName() const override;
  bool isRunning() const override;
  bool isFallback() const override;

  static bool nativeSupported();
  static QString unsupportedBadge();

 private:
  std::unique_ptr<StaticImageRenderer> fallback_;
  QImage frame_;
  bool running_ = false;
  bool paused_ = false;
};

