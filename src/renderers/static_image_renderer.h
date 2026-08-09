#pragma once

#include "renderer.h"

class StaticImageRenderer final : public Renderer {
  Q_OBJECT

 public:
  explicit StaticImageRenderer(RendererSpec spec, QObject *parent = nullptr);

  bool start(QString *error) override;
  void stop() override;
  void pause() override;
  void resume() override;
  QImage lastFrame() const override;
  QString rendererName() const override;
  bool isRunning() const override;
  bool isFallback() const override;

  static QImage fallbackFrame(const QString &label, int width, int height);
  static QImage defaultFrame(int width, int height);

 private:
  QImage frame_;
  bool running_ = false;
  bool paused_ = false;
};
