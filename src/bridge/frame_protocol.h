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
