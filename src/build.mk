# Place this custom makefile here: <project-path>/src/build.mk

INCLUDE_DIRS += $(SOURCE_PATH)/$(USRSRC)  # add user sources to include path
# add C and CPP files - if USRSRC is not empty, then add a slash
CPPSRC += $(call target_files,$(USRSRC_SLASH),*.cpp)
CSRC += $(call target_files,$(USRSRC_SLASH),*.c)

# Enable Muon Detection for SoMs (B-SoM, B-5SoM, M-SoM)
ifneq (,$(filter $(PLATFORM_ID),23 25 35))
CFLAGS += -DENABLE_MUON_DETECTION
CXXFLAGS += -DENABLE_MUON_DETECTION
endif
