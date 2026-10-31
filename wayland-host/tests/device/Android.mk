LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := trierarch-ahb-egl-test
LOCAL_SRC_FILES := ahb_egl_test.c
LOCAL_CFLAGS := -DANDROID -std=gnu11 -Wall -Wextra
LOCAL_LDLIBS := -lEGL -lGLESv2 -landroid -llog -ldl
include $(BUILD_EXECUTABLE)
