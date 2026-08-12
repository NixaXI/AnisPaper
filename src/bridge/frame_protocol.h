#pragma once

#include <QtGlobal>

// This is intentionally a plain, fixed-size ABI shared by anis-paperd and the
// Plasma QML plugin.  Do not add pointers, Qt types, or compiler packing here.
struct __attribute__((packed)) FrameHeader {
  char magic[4];
  quint32 width;
  quint32 height;
  quint64 frameNo;
  quint64 timestampNs;
  quint32 stride;
};

static_assert(sizeof(FrameHeader) == 32,
              "AnisPaper bridge FrameHeader must remain 32 bytes");

inline quint64 loadFrameNo(const FrameHeader *header) {
  if (!header) {
    return 0;
  }
  // frameNo is offset 12 in the fixed wire ABI. GCC's generic atomics support
  // this unaligned packed location without changing the serialized layout.
  quint64 value = 0;
  __atomic_load(&header->frameNo, &value, __ATOMIC_ACQUIRE);
  return value;
}

inline void storeFrameNo(FrameHeader *header, quint64 frameNo) {
  __atomic_store(&header->frameNo, &frameNo, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// Binary scene transport (anis-paper-scene-engine child -> anis-paperd).
//
// Fixed ABI shared with the non-Qt scene child, which carries an identical
// definition in src/scene_engine/main.cpp.  Single writer, single reader,
// latest-frame-wins: the header is followed by `buffers` RGBA8888 slots;
// the writer copies a frame into slot (frameNo % buffers) and then publishes
// writeIndex + frameNo with release ordering.  Readers acquire-frameNo first
// and retry once if it moved mid-copy.
constexpr char kSceneTransportMagic[4] = {'A', 'N', 'S', 'T'};
constexpr quint32 kSceneTransportVersion = 1;
constexpr quint32 kSceneTransportFormatRgba8888 = 1;

struct SceneTransportHeader {
  char magic[4];
  quint32 version;
  quint64 frameNo;
  quint64 timestampNs;
  quint32 writeIndex;
  quint32 width;
  quint32 height;
  quint32 stride;
  quint32 format;
  quint32 buffers;
  quint32 reserved[4];
};

static_assert(sizeof(SceneTransportHeader) == 64,
              "AnisPaper scene transport header must remain 64 bytes");

inline quint64 loadSceneFrameNo(const SceneTransportHeader *header) {
  quint64 value = 0;
  __atomic_load(&header->frameNo, &value, __ATOMIC_ACQUIRE);
  return value;
}

inline quint32 loadSceneWriteIndex(const SceneTransportHeader *header) {
  quint32 value = 0;
  __atomic_load(&header->writeIndex, &value, __ATOMIC_ACQUIRE);
  return value;
}
