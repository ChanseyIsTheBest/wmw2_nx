#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	wmw2_nx
APP_TITLE	:=	Where's My Water? 2
APP_AUTHOR	:=	ChanseyIsTheBest
APP_VERSION	:=	1.0.0
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:= $(CFLAGS)

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# GLESv1_CM is the fixed-function pipeline libwalaber.so renders with -- it
# imports 56 GL entry points, all GLES 1.1 plus GL_OES_framebuffer_object, and
# zero GLES2 shader entry points. EGL/glapi/drm_nouveau come along with mesa.
#
# -lz is required: libwalaber.so imports zlib directly (crc32/deflate/inflate)
# for its archive reader. libpng backs nx_pointer's optional cursor.png.
#
# No FreeType/ffmpeg/dav1d: the engine decodes its own WebP art and renders its
# own fonts. The eight .mp4 files under assets/Water/Movies/ are deliberately
# NOT decoded -- see wmw2_jni.c.
#
# SDL2 backs the OpenSL ES shim. This FMOD build ships both an OpenSL ES and an
# AudioTrack output, but the engine hard-selects setOutput(21) =
# FMOD_OUTPUTTYPE_OPENSL with no device query and no fallback, so OpenSL is the
# only path that runs. See opensles.c.
# GLESv1_CM ONLY -- do not add -lGLESv2. Both are static archives that define
# the shared core entry points (glClear, glBindTexture, glViewport, ...), so
# linking them together is a wall of "multiple definition" errors. The engine is
# GLES 1.1, the cursor overlay is fixed-function, and the *OES extensions are
# resolved at runtime through eglGetProcAddress -- nothing here needs GLESv2.
# ffmpeg backs the cutscene player (source/wmw2_movie.c), and only that.
#
#   dkp-pacman -S switch-ffmpeg      (Windows/msys2: pacman -S switch-ffmpeg)
#
# ASK pkg-config RATHER THAN HARDCODING THE DEPENDENCY LIST. switch-ffmpeg is a
# static build, so every one of its own dependencies has to appear on our link
# line too, and that list belongs to the package, not to this port. --static
# gives the full transitive chain; hardcoding it means a package update breaks
# the link with undefined symbols from inside libavcodec.
#
# Set WMW2_VIDEO to 0 in source/config.h to build without any of this.
WMW2_VIDEO_ON := $(shell grep -sE 'define[[:space:]]+WMW2_VIDEO[[:space:]]+1' source/config.h)
ifneq ($(strip $(WMW2_VIDEO_ON)),)
ifeq ($(wildcard $(PORTLIBS)/include/libavformat/avformat.h),)
$(warning ==============================================================)
$(warning  switch-ffmpeg is NOT INSTALLED, and WMW2_VIDEO is 1 in config.h)
$(warning )
$(warning  looked for: $(PORTLIBS)/include/libavformat/avformat.h)
$(warning )
$(warning  Install it -- from the devkitPro msys2 shell on Windows:)
$(warning      pacman -S switch-ffmpeg)
$(warning  or on linux/macOS:)
$(warning      dkp-pacman -S switch-ffmpeg)
$(warning )
$(warning  Or build without cutscenes: set WMW2_VIDEO to 0 in)
$(warning  source/config.h. wmw2_movie.c then compiles to empty stubs)
$(warning  needing no ffmpeg header and no ffmpeg library, and movies are)
$(warning  skipped exactly as they were before the feature existed.)
$(warning ==============================================================)
$(error switch-ffmpeg missing -- see above)
endif
endif

PKGCONF     := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
FFMPEG_PKGS := libavformat libavcodec libswresample libswscale libavutil
FFMPEG_LIBS := $(shell $(PKGCONF) --static --libs $(FFMPEG_PKGS) 2>/dev/null)
ifeq ($(strip $(FFMPEG_LIBS)),)
FFMPEG_LIBS := -lavformat -lavcodec -lswresample -lswscale -lavutil \
               -ldav1d -lass -lfribidi -lharfbuzz -lfreetype -lbz2
endif

LIBS	:= -lSDL2 -lGLESv1_CM -lEGL -lglapi -ldrm_nouveau \
	   -Wl,--start-group $(FFMPEG_LIBS) -Wl,--end-group \
	   -lpng -lbz2 -lz -lnx -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
