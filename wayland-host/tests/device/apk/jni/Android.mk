LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := trierarch_scanout_test
LOCAL_SRC_FILES := scanout_jni.c
LOCAL_CFLAGS := -DANDROID -std=gnu11 -Wall -Wextra
LOCAL_LDLIBS := -landroid -llog -lEGL -lGLESv2
include $(BUILD_SHARED_LIBRARY)
