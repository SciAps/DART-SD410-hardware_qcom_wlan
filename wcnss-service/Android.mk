ifneq (,$(filter arm aarch64 arm64, $(TARGET_ARCH)))
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := wcnss_service
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/common/inc/
LOCAL_SRC_FILES := wcnss_service.c
ifeq ($(strip $(TARGET_USES_QCOM_WCNSS_QMI)),true)
LOCAL_CFLAGS += -DWCNSS_QMI
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/qmi/inc
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/qmi/services
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/qmi/platform
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/qmi/src
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/qmi/core/lib/inc
LOCAL_SRC_FILES += wcnss_qmi_client.c
endif #TARGET_USES_QCOM_WCNSS_QMI
LOCAL_SHARED_LIBRARIES := libc libcutils libutils liblog
ifeq ($(strip $(TARGET_USES_QCOM_WCNSS_QMI)),true)
LOCAL_SHARED_LIBRARIES += libidl libdiag libdsutils libqmiservices libqmi libqcci_legacy libqmi_client_qmux
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/libmdmdetect/inc
LOCAL_SHARED_LIBRARIES += libmdmdetect
endif #TARGET_USES_QCOM_WCNSS_QMI
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += -Wall
include $(BUILD_EXECUTABLE)
endif
