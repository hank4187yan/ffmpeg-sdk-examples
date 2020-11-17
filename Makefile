FFMPEG_INSTALL_DIR=/data/PJT-ffmpeg/PJT-ffmpeg-4.2.4.for-study/ffmpeg-4.2.4.installed
LIB_DIR=$(FFMPEG_INSTALL_DIR)/lib
INC_DIR=$(FFMPEG_INSTALL_DIR)/include

CC = gcc
CXX = g++

CFLAGS := -Wall -g -fPIC -lrt -lz -lpthread -D_GNU_SOURCE  -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS
CFLAGS += $(addprefix -I, $(INC_DIR))

#
FFMPEG_LIBS=    avformat                        \
		avdevice                        \
                avformat                        \
                avcodec                         \
                avfilter                        \
                avutil                          \
                avformat                        \
                avutil                          \
                swscale                         \
		postproc                        \
                avfilter                        \
                avcodec                         \
                swresample                      \
                swscale                         \
                avutil                          \
		postproc                        \
                avfilter                        \
		fdk-aac                         \
		x264                            \

LDLIBS := $(addprefix -L, $(LIB_DIR)) $(addprefix -l, $(FFMPEG_LIBS))  -lm -lpthread -ldl -lrt -lz -lm -lstdc++

EXAMPLES=       avio_dir_cmd                       \
                avio_reading                       \
                decode_audio                       \
                decode_video                       \
                demuxing_decoding                  \
		demuxing_insert_sps_pps            \
                encode_audio                       \
                encode_video                       \
                extract_mvs                        \
                filtering_video                    \
                filtering_audio                    \
                http_multiclient                   \
                hw_decode                          \
                metadata                           \
                muxing                             \
                remuxing                           \
                resampling_audio                   \
                scaling_video                      \
		simple-memory-transcoder           \
                transcode_aac                      \
                transcoding                        \
		vaapi_encode                       \
		vaapi_transcode                    \

OBJS=$(addsuffix .o,$(EXAMPLES))

# the following examples make explicit use of the math library
avcodec:           LDLIBS += -lm
encode_audio:      LDLIBS += -lm
muxing:            LDLIBS += -lm
resampling_audio:  LDLIBS += -lm

.phony: all clean-test clean

all: $(OBJS) $(EXAMPLES)

$(MAIN_SO): $(LV_SO_OBJS)
	$(LD) $@ $^ $(LIB)
	

%.o: %.cpp
	$(CXX) $(CFLAGS) $(INC) -c -o $@ $<

%.o: %.cxx
	$(CXX) $(CFLAGS) $(INC) -c -o $@ $<

%.o: %.c
	$(CXX) $(CFLAGS) $(INC) -c -o $@ $<

%.o: %.cc
	$(CXX) $(CFLAGS) $(INC) -c -o $@ $<

%.o: %.c
	$(CXX) $(CFLAGS) $(INC) -c -o $@ $<

clean-test:
	$(RM) test*.pgm test.h264 test.mp2 test.sw test.mpg

clean: clean-test
	$(RM) $(EXAMPLES) $(OBJS)
