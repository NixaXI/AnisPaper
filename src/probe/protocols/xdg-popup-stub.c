/*
 * zwlr_layer_surface_v1 has a get_popup request whose signature references the
 * external xdg_popup interface. This probe never sends that request, but the
 * generated layer-shell metadata still needs a linkable interface descriptor.
 */
#include <wayland-client-core.h>

const struct wl_interface xdg_popup_interface = {
    "xdg_popup", 1, 0, 0, 0, 0,
};
