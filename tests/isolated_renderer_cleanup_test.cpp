#include "../src/bridge/frame_protocol.h"
#include "../src/renderers/isolated_renderer.h"

#include <QCoreApplication>

#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr quint32 kWidth = 64;
constexpr quint32 kHeight = 64;
constexpr quint32 kStride = kWidth * 4;
constexpr quint32 kBuffers = 3;

bool require(bool value, const char *message) {
  if (!value) std::fprintf(stderr, "isolated_renderer_cleanup_test: %s\n", message);
  return value;
}

class Transport final {
 public:
  Transport() {
    name_ = QByteArray("/anispaper-isolated-cleanup-") + QByteArray::number(::getpid());
    ::shm_unlink(name_.constData());
    fd_ = ::shm_open(name_.constData(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd_ < 0) return;
    const size_t size = sizeof(SceneTransportHeader) + size_t(kStride) * kHeight * kBuffers;
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) return;
    mapping_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapping_ == MAP_FAILED) {
      mapping_ = nullptr;
      return;
    }
    size_ = size;
    auto *header = static_cast<SceneTransportHeader *>(mapping_);
    std::memset(header, 0, sizeof(*header));
    std::memcpy(header->magic, kSceneTransportMagic, 4);
    header->version = kSceneTransportVersion;
    header->width = kWidth;
    header->height = kHeight;
    header->stride = kStride;
    header->format = kSceneTransportFormatRgba8888;
    header->buffers = kBuffers;
  }

  ~Transport() {
    if (mapping_) ::munmap(mapping_, size_);
    if (fd_ >= 0) ::close(fd_);
    if (!name_.isEmpty()) ::shm_unlink(name_.constData());
  }

  bool valid() const { return fd_ >= 0 && mapping_ != nullptr; }
  QString name() const { return QString::fromLocal8Bit(name_); }

 private:
  QByteArray name_;
  int fd_ = -1;
  void *mapping_ = nullptr;
  size_t size_ = 0;
};
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  Transport transport;
  bool ok = require(transport.valid(), "could not construct source transport");
  RendererSpec spec;
  spec.type = QStringLiteral("scene");
  IsolatedRenderer renderer(spec);
  ok = require(renderer.openSceneTransportForTesting(transport.name()),
               "renderer did not map synthetic transport") && ok;
  ok = require(renderer.hasSceneTransportForTesting(), "transport was not retained before stop") && ok;

  // No child is ever started, so QProcess is already NotRunning: this is the
  // same branch taken after an unexpected child exit before the manager deletes
  // the renderer.  stop() must still close the mapping/fd and be idempotent.
  renderer.stop();
  ok = require(!renderer.hasSceneTransportForTesting(),
               "NotRunning stop leaked the scene transport mapping/fd") && ok;
  renderer.stop();
  ok = require(!renderer.hasSceneTransportForTesting(),
               "second NotRunning stop was not idempotent") && ok;
  if (ok) std::puts("isolated_renderer_cleanup_test: PASS");
  return ok ? 0 : 1;
}
