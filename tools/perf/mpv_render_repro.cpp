// Minimal libmpv/OpenGL reproducer for the video-memory investigation.
// Deliberately has no AnisPaper, Qt Quick, SHM, RPC, JPEG or Plasma code.

#include <QCoreApplication>
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QThread>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale>
#include <string>

namespace {

struct Options {
  QString file;
  int width = 1920;
  int height = 1080;
  int fps = 60;
  int seconds = 600;
  bool readback = true;
  bool reportSwap = false;
  bool updateDriven = false;
  bool loop = false;
};

void usage(const char *argv0) {
  std::fprintf(stderr,
               "Usage: %s --file PATH [--seconds N] [--fps N] [--width N] "
               "[--height N] [--skip-readback] [--report-swap] [--update-driven] [--loop]\n",
               argv0);
}

bool parseInt(const char *text, int *value) {
  if (!text || !value) {
    return false;
  }
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed < 1 ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

bool parseArgs(int argc, char **argv, Options *options) {
  if (!options) {
    return false;
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--file") {
      if (i + 1 >= argc) {
        return false;
      }
      options->file = QString::fromLocal8Bit(argv[++i]);
      continue;
    }
    int *target = nullptr;
    if (arg == "--seconds") {
      target = &options->seconds;
    } else if (arg == "--fps") {
      target = &options->fps;
    } else if (arg == "--width") {
      target = &options->width;
    } else if (arg == "--height") {
      target = &options->height;
    }
    if (target) {
      if (i + 1 >= argc || !parseInt(argv[++i], target)) {
        return false;
      }
      continue;
    }
    if (arg == "--skip-readback") {
      options->readback = false;
      continue;
    }
    if (arg == "--report-swap") {
      options->reportSwap = true;
      continue;
    }
    if (arg == "--update-driven") {
      options->updateDriven = true;
      continue;
    }
    if (arg == "--loop") {
      options->loop = true;
      continue;
    }
    usage(argv[0]);
    return false;
  }
  return !options->file.isEmpty() && options->fps <= 240 && options->width <= 7680 &&
         options->height <= 4320;
}

void *getProcAddress(void *ctx, const char *name) {
  auto *context = static_cast<QOpenGLContext *>(ctx);
  return context ? reinterpret_cast<void *>(context->getProcAddress(name)) : nullptr;
}

void pumpEvents(mpv_handle *handle) {
  while (handle) {
    const mpv_event *event = mpv_wait_event(handle, 0.0);
    if (!event || event->event_id == MPV_EVENT_NONE) {
      return;
    }
  }
}

void updateCallback(void *ctx) {
  auto *pending = static_cast<std::atomic_bool *>(ctx);
  if (pending) {
    pending->store(true, std::memory_order_release);
  }
}

bool setOption(mpv_handle *handle, const char *name, const char *value) {
  return mpv_set_option_string(handle, name, value) >= 0;
}

}  // namespace

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  Options options;
  if (!parseArgs(argc, argv, &options)) {
    usage(argv[0]);
    return 2;
  }
  std::setlocale(LC_NUMERIC, "C");

  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  if (!surface.isValid()) {
    std::fprintf(stderr, "offscreen surface unavailable\n");
    return 3;
  }
  QOpenGLContext context;
  context.setFormat(surface.format());
  if (!context.create() || !context.makeCurrent(&surface)) {
    std::fprintf(stderr, "OpenGL context unavailable\n");
    return 3;
  }
  QOpenGLFramebufferObjectFormat fboFormat;
  fboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
  fboFormat.setInternalTextureFormat(GL_RGBA8);
  QOpenGLFramebufferObject fbo(options.width, options.height, fboFormat);
  if (!fbo.isValid()) {
    std::fprintf(stderr, "FBO unavailable\n");
    return 3;
  }

  mpv_handle *mpv = mpv_create();
  if (!mpv || !setOption(mpv, "vo", "libmpv") ||
      !setOption(mpv, "terminal", "no") || !setOption(mpv, "audio", "no") ||
      !setOption(mpv, "hwdec", "no") || !setOption(mpv, "keep-open", "yes") ||
      mpv_initialize(mpv) < 0) {
    std::fprintf(stderr, "mpv initialization failed\n");
    if (mpv) {
      mpv_terminate_destroy(mpv);
    }
    return 4;
  }

  mpv_opengl_init_params glInit{};
  glInit.get_proc_address = &getProcAddress;
  glInit.get_proc_address_ctx = &context;
  mpv_render_param createParams[] = {
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  mpv_render_context *renderContext = nullptr;
  if (mpv_render_context_create(&renderContext, mpv, createParams) < 0) {
    std::fprintf(stderr, "mpv render context creation failed\n");
    mpv_terminate_destroy(mpv);
    return 4;
  }
  std::atomic_bool framePending{true};
  if (options.updateDriven) {
    mpv_render_context_set_update_callback(renderContext, &updateCallback,
                                            &framePending);
  }
  const QByteArray file = QFileInfo(options.file).absoluteFilePath().toUtf8();
  const char *load[] = {"loadfile", file.constData(), "replace", nullptr};
  if (mpv_command(mpv, load) < 0) {
    std::fprintf(stderr, "mpv loadfile failed\n");
    mpv_render_context_free(renderContext);
    mpv_terminate_destroy(mpv);
    return 4;
  }
  mpv_set_property_string(mpv, "loop-file", options.loop ? "inf" : "no");

  QByteArray pixels;
  if (options.readback) {
    pixels.resize(options.width * options.height * 4);
  }
  QElapsedTimer elapsed;
  elapsed.start();
  const qint64 deadline = static_cast<qint64>(options.seconds) * 1000;
  const qint64 frameInterval = std::max<qint64>(1, 1000 / options.fps);
  qint64 nextFrame = 0;
  quint64 frames = 0;
  std::fprintf(stdout, "start width=%d height=%d fps=%d seconds=%d readback=%d report_swap=%d update_driven=%d loop=%d\n",
               options.width, options.height, options.fps, options.seconds,
               options.readback ? 1 : 0, options.reportSwap ? 1 : 0,
               options.updateDriven ? 1 : 0, options.loop ? 1 : 0);
  std::fflush(stdout);
  while (elapsed.elapsed() < deadline) {
    const qint64 now = elapsed.elapsed();
    if (now < nextFrame) {
      QThread::msleep(static_cast<unsigned long>(std::min<qint64>(nextFrame - now, 5)));
      continue;
    }
    nextFrame += frameInterval;
    pumpEvents(mpv);
    if (options.updateDriven && !framePending.exchange(false, std::memory_order_acq_rel)) {
      continue;
    }
    if (options.updateDriven &&
        (mpv_render_context_update(renderContext) & MPV_RENDER_UPDATE_FRAME) == 0) {
      continue;
    }
    fbo.bind();
    auto *gl = context.functions();
    gl->glViewport(0, 0, options.width, options.height);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    int flipY = 1;
    mpv_opengl_fbo target{static_cast<int>(fbo.handle()), options.width, options.height,
                          GL_RGBA8};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(renderContext, params);
    fbo.bind();
    if (options.readback) {
      gl->glReadPixels(0, 0, options.width, options.height, GL_RGBA,
                       GL_UNSIGNED_BYTE, pixels.data());
    }
    fbo.release();
    if (options.reportSwap) {
      mpv_render_context_report_swap(renderContext);
    }
    ++frames;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
  }
  const double seconds = elapsed.elapsed() / 1000.0;
  std::fprintf(stdout, "done frames=%llu elapsed=%.3f effective_fps=%.3f\n",
               static_cast<unsigned long long>(frames), seconds,
               seconds > 0.0 ? static_cast<double>(frames) / seconds : 0.0);
  std::fflush(stdout);
  mpv_render_context_free(renderContext);
  mpv_terminate_destroy(mpv);
  context.doneCurrent();
  return 0;
}
