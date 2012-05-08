ifneq ($(TARGET_SIMULATOR),true)

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE_TAGS := eng development
LOCAL_SRC_FILES := main.cpp CameraNativeWindow.cpp
LOCAL_MODULE := snapshot
LOCAL_STATIC_LIBRARIES := libcutils libc
LOCAL_SHARED_LIBRARIES := libhardware libcamera_client libbinder libui libdl libutils
LOCAL_CFLAGS := -g -O0
# LOCAL_LDLIBS := -ldl
include $(BUILD_EXECUTABLE)

endif  # TARGET_SIMULATOR != true

