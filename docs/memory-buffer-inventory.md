# AnisPaper — inventario de buffers de memoria

Este inventario describe el estado actual del transporte a 1920×1080 (13-08-2026).
Los tamaños son payload puro salvo donde se indica el header.

| Propietario | Propósito | Dimensiones/formato | Tamaño | Cantidad/lifetime |
|---|---|---:|---:|---|
| `FrameBridge` daemon | SHM que consume Plasma por output | 1920×1080 RGBA8888 | 8.294.400 B payload + 32 B `FrameHeader` = 8.294.432 B | 1 por output, vive mientras el wallpaper está aplicado |
| Scene engine child | Transporte producer→daemon | 1920×1080 RGBA8888 | 3×8.294.400 B + 64 B `SceneTransportHeader` = 24.883.264 B | 3 slots, vive mientras el child scene está activo |
| Scene engine child | PBO/readback scratch | textura RGBA + 3 PBO | 4×8.294.400 B nominales | 1 textura + 3 PBO, vive durante el child; la GPU puede contabilizarlo aparte |
| Scene daemon `frame_` | Copia privada del slot scene para consumidores Qt | 1920×1080 RGBA8888 | 8.294.400 B | 1 por renderer scene, reutilizada por frame |
| Video child `frame_` | destino de `glReadPixels` | 1920×1080 RGB888 compacto (RGBA8888 sólo si el stride no es compacto) | 6.220.800 B nominales RGB | 1, reutilizada; las filas se intercambian con un scratch de una fila |
| Video child FBO | textura GL libmpv | 1920×1080 RGBA8 | 8.294.400 B nominales | 1, vive durante el renderer |
| IsolatedRenderer JPEG path | JPEG/base64, JSON y `decoded` | depende del frame | transitorio, no persistente | se crea/libera por notificación; no es SHM |
| Plasma provider | `mmap` del bridge + `QImage::copy()` | 1920×1080 RGBA8888 premultiplied | 8.294.400 B por output | copia cacheada por output; el mapping se reutiliza mientras el nombre SHM sea válido |
| QML scene graph | textura subida desde la imagen | depende del backend Qt Quick | no observable desde `/proc` del daemon | gestionado por Plasma; no se atribuye automáticamente al daemon |
| Preview RPC | snapshot del bridge + JPEG/base64 | frame completo | transitorio por petición | sólo al pedir `preview.frame`; no participa en el stream normal |

La medición real de `/dev/shm` coincide con esta tabla: dos bridges físicos
(2×8.294.432) y un transporte scene (24.883.264), total **41.472.128 B**.
No se observaron objetos extra al finalizar la campaña de reemplazos.

El número exacto de AVFrame/packet surfaces internas de libmpv/FFmpeg no está
expuesto por esta API. El perfil aislado mostró aproximadamente 208 MiB vivos
en `av_malloc` a 640×360 y el soak real mostró crecimiento del PSS del hijo de
vídeo (~33,6 MiB/h); no se redujeron colas a ciegas y la retención de libmpv
queda abierta.

Copias de un frame normal scene: PBO→slot scene, slot→`frame_` daemon,
`frame_`→FrameBridge y provider mmap→QImage; son fronteras de proceso/lifetime.
Video añade readback RGB, JPEG/base64, decode/conversión y las mismas fronteras
bridge/provider. RGB888 y el flip por filas reducen allocations temporales, pero
no eliminan la retención interna de libmpv. Esta sigue siendo la oportunidad
futura, no un SHM huérfano: el apagado SIGTERM ya limpia transports y bridges.
