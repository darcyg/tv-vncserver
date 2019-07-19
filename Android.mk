LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := tv-vncserver
LOCAL_C_INCLUDES += external/libvncserver external/zlib 
LOCAL_SRC_FILES := tv-vncserver.cpp
LOCAL_STATIC_LIBRARIES := libvncserver libpng libjpeg libz 
LOCAL_SHARED_LIBRARIES := libui  libcutils liblog libutils libgui 
LOCAL_SHARED_LIBRARIES += libhidlbase libhidltransport libbinder android.hardware.graphics.mapper@2.0 android.hardware.tv.cec@1.0
LOCAL_CFLAGS := -Wno-unused-parameter
#LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := tv-vncserver.rc
LOCAL_MODULE_CLASS :=  ETC
LOCAL_MODULE_RELATIVE_PATH := init
LOCAL_SRC_FILES := tv-vncserver.rc
#LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_PREBUILT)
