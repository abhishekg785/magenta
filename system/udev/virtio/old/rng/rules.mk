LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/virtio-rng.c \

MODULE_DEPS += \
	dev/virtio \
	dev/hw_rng \
	lib/cbuf

include make/module.mk
