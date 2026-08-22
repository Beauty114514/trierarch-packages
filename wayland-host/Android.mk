LOCAL_PATH := $(call my-dir)
WAYLAND_INSTALL := $(LOCAL_PATH)/build/wayland-install
WAYLAND_INCLUDE := $(WAYLAND_INSTALL)/include
WAYLAND_LIB := $(WAYLAND_INSTALL)/lib
PROTO_DIR := $(LOCAL_PATH)/protocol/generated

include $(CLEAR_VARS)
LOCAL_MODULE := wayland-server
LOCAL_SRC_FILES := $(WAYLAND_LIB)/libwayland-server.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := ffi
LOCAL_SRC_FILES := $(WAYLAND_LIB)/libffi.so
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := trierarch-wayland-host
LOCAL_SRC_FILES := src/compositor.c src/output.c src/seat.c src/pointer_constraints.c src/presentation.c src/buffer_shm.c src/linux_dmabuf.c src/egl_buffer.c src/renderer.c src/single_pixel_buffer.c src/viewporter.c src/surface.c src/subcompositor.c src/xdg_shell.c src/xdg_output.c src/xdg_decoration.c src/protocol_stubs.c src/jni_bridge.c $(PROTO_DIR)/xdg-shell-protocol.c $(PROTO_DIR)/linux-dmabuf-v1-protocol.c $(PROTO_DIR)/single-pixel-buffer-v1-protocol.c $(PROTO_DIR)/viewporter-protocol.c $(PROTO_DIR)/pointer-constraints-protocol.c $(PROTO_DIR)/presentation-time-protocol.c $(PROTO_DIR)/xdg-output-unstable-v1-protocol.c $(PROTO_DIR)/xdg-decoration-unstable-v1-protocol.c $(PROTO_DIR)/fractional-scale-v1-protocol.c $(PROTO_DIR)/relative-pointer-unstable-v1-protocol.c $(PROTO_DIR)/android-wlegl-protocol.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/src $(PROTO_DIR) $(WAYLAND_INCLUDE)
LOCAL_CFLAGS := -DANDROID -std=gnu11 -fPIC
LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv2
LOCAL_SHARED_LIBRARIES := wayland-server ffi
include $(BUILD_SHARED_LIBRARY)
