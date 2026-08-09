#pragma once

#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QString>

// The renderer contract is deliberately small.  It is shared by the isolated
// video/web workers and by the in-daemon static fallback.  All calls are made
// from the Qt main thread; implementations must not start their own watchdog
// thread.
struct RendererSpec {
  QString id;
  QString type;
  QString file;
  QString preview;
  QString output;
  QJsonObject properties;
  int fps = 30;
  double volume = 1.0;
  double speed = 1.0;
  bool loop = true;
  int width = 640;
  int height = 360;
};

class Renderer : public QObject {
  Q_OBJECT

 public:
  explicit Renderer(RendererSpec spec, QObject *parent = nullptr)
      : QObject(parent), spec_(std::move(spec)) {}
  ~Renderer() override = default;

  virtual bool start(QString *error) = 0;
  virtual void stop() = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual QImage lastFrame() const = 0;
  virtual QString rendererName() const = 0;
  virtual bool isRunning() const = 0;
  virtual bool isFallback() const { return false; }
  virtual qint64 processId() const { return 0; }
  virtual double frameRate() const { return 0.0; }

  const RendererSpec &spec() const { return spec_; }

 signals:
  void ready();
  void frameReady(const QImage &frame);
  void fatal(const QString &reason);

 protected:
  RendererSpec spec_;
};
