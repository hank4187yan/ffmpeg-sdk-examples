/*
 * Copyright (c) 2012 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * Demuxing and decoding example.
 *
 * Show how to use the libavformat and libavcodec API to demux audio and video data.
 * 对于视频的H.264数据，解析后，对每个IDR帧前插入一个SPS和PPS单元；
 * @example demuxing_and_insert_sps_pps.cpp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>

#ifdef __cplusplus
}
#endif

using namespace std;


static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *video_dec_ctx = NULL, *audio_dec_ctx;
static AVStream *video_stream = NULL, *audio_stream = NULL;

static const char *src_filename = NULL;
static int video_stream_idx = -1, audio_stream_idx = -1;

static AVPacket pkt;


/* Enable or disable frame reference counting. You are not supposed to support
 * both paths in your application but pick the one most appropriate to your
 * needs. Look for the use of refcount in this example to see what are the
 * differences of API usage between them. */
static int refcount = 0;


typedef struct AVCDecoderConfiguration {   
	unsigned int configurationVersion;   
	unsigned int AVCProfileIndication;   
	unsigned int profile_compatibility;   
	unsigned int AVCLevelIndication;   

	unsigned int lengthSizeMinusOne;   

	unsigned int numOfSequenceParameterSets;   
	unsigned int sequenceParameterSetLength;
	unsigned char sequenceParameterSetNALUnit[1024];

	unsigned int numPictureParameterSets;
	unsigned int pictureParameterSetLength;
	unsigned char pictureParameterSetNALUnit[1024];
} AVCDecoderConfiguration;


int parse_avc_configuration(unsigned char* buf,  int32_t buf_size,
		AVCDecoderConfiguration* avc_dec_conf){
	int32_t ret = 0;
	unsigned char* buf_ptr = buf;
	
	int32_t        avc_nal_start_code_size = 4;
	unsigned char  avc_nal_start_code[4] = {0, 0, 0, 1};


	avc_dec_conf->configurationVersion = (unsigned char)buf[0];
	avc_dec_conf->AVCProfileIndication = (unsigned char)buf[1];
	avc_dec_conf->profile_compatibility= (unsigned char)buf[2];
	avc_dec_conf->AVCLevelIndication   = (unsigned char)buf[3];
	
	avc_dec_conf->lengthSizeMinusOne   = (unsigned char)(buf[4]&0x03);


	// SPS
	avc_dec_conf->numOfSequenceParameterSets = (unsigned char)(buf[5]&0x1f);

	avc_dec_conf->sequenceParameterSetLength = (buf[6] << 8 | buf[7]);	
	buf_ptr += 8;	
	memcpy(avc_dec_conf->sequenceParameterSetNALUnit,  avc_nal_start_code, avc_nal_start_code_size);
	memcpy(&avc_dec_conf->sequenceParameterSetNALUnit[4], buf_ptr, avc_dec_conf->sequenceParameterSetLength);

	buf_ptr += avc_dec_conf->sequenceParameterSetLength;
	avc_dec_conf->sequenceParameterSetLength += 4;




	// PPS
	avc_dec_conf->numPictureParameterSets = *buf_ptr;
	buf_ptr += 1;
	
	avc_dec_conf->pictureParameterSetLength = (buf_ptr[0] << 8 | buf_ptr[1]);
	buf_ptr += 2;

	memcpy(avc_dec_conf->pictureParameterSetNALUnit, avc_nal_start_code, avc_nal_start_code_size);
	memcpy(&avc_dec_conf->pictureParameterSetNALUnit[4], buf_ptr, avc_dec_conf->pictureParameterSetLength);
	buf_ptr += avc_dec_conf->pictureParameterSetLength;
	avc_dec_conf->pictureParameterSetLength += 4;
	
	
	return ret;
}






static int alloc_and_copy(AVPacket *out,
                          const uint8_t *sps_pps, uint32_t sps_pps_size,
                          const uint8_t *in, uint32_t in_size, int ps)
{
    uint32_t offset         = out->size;
    uint8_t start_code_size = offset == 0 || ps ? 4 : 3;
    int err;

    err = av_grow_packet(out, sps_pps_size + in_size + start_code_size);
    if (err < 0)
        return err;

    if (sps_pps)
        memcpy(out->data + offset, sps_pps, sps_pps_size);
    memcpy(out->data + sps_pps_size + start_code_size + offset, in, in_size);
    if (start_code_size == 4) {
        AV_WB32(out->data + offset + sps_pps_size, 1);
    } else {
        (out->data + offset + sps_pps_size)[0] =
        (out->data + offset + sps_pps_size)[1] = 0;
        (out->data + offset + sps_pps_size)[2] = 1;
    }

    return 0;
}

#define PKT_LENGTH_SIZE  4
typedef struct H264BSFContext {
    int32_t  sps_offset;
    int32_t  pps_offset;
    uint8_t  length_size;
    uint8_t  new_idr;
    uint8_t  idr_sps_seen;
    uint8_t  idr_pps_seen;
    int      extradata_parsed;
} H264BSFContext;


static int h264_mp4toannexb_filter(AVPacket *in,  
		unsigned char* sps_pps_data, int32_t sps_pps_size,
		AVPacket *out){

    uint8_t unit_type;
    int32_t nal_size;
    uint32_t cumul_size    = 0;

    const uint8_t *buf;
    const uint8_t *buf_end;
    int            buf_size;
    int ret = 0, i;


	H264BSFContext  h264_bsf_ctx;
	memset(&h264_bsf_ctx, 0, sizeof(H264BSFContext));
	H264BSFContext  *s = &h264_bsf_ctx;
	s->length_size = PKT_LENGTH_SIZE;



    buf      = in->data;
    buf_size = in->size;
    buf_end  = in->data + in->size;
	

    do {
        ret= AVERROR(EINVAL);
        if (buf + PKT_LENGTH_SIZE > buf_end)
            goto fail;

        for (nal_size = 0, i = 0; i<PKT_LENGTH_SIZE; i++)
            nal_size = (nal_size << 8) | buf[i];

        buf += PKT_LENGTH_SIZE;
        unit_type = *buf & 0x1f;

        if (nal_size > buf_end - buf || nal_size < 0)
            goto fail;

        if (unit_type == 7){
            s->idr_sps_seen = s->new_idr = 1;
		}
        else if (unit_type == 8) {
            s->idr_pps_seen = s->new_idr = 1;
            /* if SPS has not been seen yet, prepend the AVCC one to PPS */
            if (!s->idr_sps_seen) {
                if (s->sps_offset == -1) {
                    printf("SPS not present in the stream, nor in AVCC, stream may be unreadable\n");
				} else {
                    ret = alloc_and_copy(out,
                                         //ctx->par_out->extradata + s->sps_offset,
                                         //s->pps_offset != -1 ? s->pps_offset : ctx->par_out->extradata_size - s->sps_offset,
										 sps_pps_data, sps_pps_size,
                                         buf, nal_size, 1);
					if (ret < 0){
                        goto fail;
					}
                    s->idr_sps_seen = 1;
                    goto next_nal;
                }
            }
        }

        /* if this is a new IDR picture following an IDR picture, reset the idr flag.
         * Just check first_mb_in_slice to be 0 as this is the simplest solution.
         * This could be checking idr_pic_id instead, but would complexify the parsing. */
        if (!s->new_idr && unit_type == 5 && (buf[1] & 0x80))
            s->new_idr = 1;

        /* prepend only to the first type 5 NAL unit of an IDR picture, if no sps/pps are already present */
        if (s->new_idr && unit_type == 5 && !s->idr_sps_seen && !s->idr_pps_seen) {
            ret=alloc_and_copy(out,
                               //ctx->par_out->extradata, ctx->par_out->extradata_size,
                               sps_pps_data, sps_pps_size,
                               buf, nal_size, 1);
			if (ret < 0){
                goto fail;
			}
            s->new_idr = 0;
        /* if only SPS has been seen, also insert PPS */
        } else if (s->new_idr && unit_type == 5 && s->idr_sps_seen && !s->idr_pps_seen) {
            if (s->pps_offset == -1) {
                printf("PPS not present in the stream, nor in AVCC, stream may be unreadable\n");
                ret = alloc_and_copy(out, NULL, 0, buf, nal_size, 0);
				if (ret < 0){
                    goto fail;
				}
            } else {
				ret = alloc_and_copy(out,
                                     //ctx->par_out->extradata + s->pps_offset, ctx->par_out->extradata_size - s->pps_offset,
									 sps_pps_data, sps_pps_size,
                                     buf, nal_size, 1);
				if (ret < 0) {
					goto fail;
				}
			}
        } else {
            ret=alloc_and_copy(out, NULL, 0, buf, nal_size, unit_type == 7 || unit_type == 8);
			if (ret < 0){
                goto fail;
			}
            if (!s->new_idr && unit_type == 1) {
                s->new_idr = 1;
                s->idr_sps_seen = 0;
                s->idr_pps_seen = 0;
            }
        }

next_nal:
        buf        += nal_size;
        cumul_size += nal_size + PKT_LENGTH_SIZE;
    } while (cumul_size < buf_size);

    ret = av_packet_copy_props(out, in);
    if (ret < 0)
        goto fail;

fail:
    if (ret < 0)
        av_packet_unref(out);

    return ret;
}



static int save_packet(AVFormatContext* fmt_ctx, AVPacket& pkt, 
			unsigned char* sps_pps_data,  int32_t sps_pps_size,
			FILE* video_pkt_fp, FILE* audio_pkt_fp,
			int is_first_pkt) {
	int ret = 0;
	int pkt_size = pkt.size;

	if (pkt.stream_index == video_stream_idx) {


		
		AVPacket opkt = {0};
		av_init_packet(&opkt);

		ret = h264_mp4toannexb_filter(&pkt,  sps_pps_data, sps_pps_size, &opkt);
		if (ret < 0) {
			printf("Error at h264_mp4toannexb_filter\n");
		}


		printf("Write pkt_size=%d into file\n",opkt.size);
		int idx = 0;
		printf("opkt.data=[ ");
		for (idx = 0; idx < (opkt.size < 32 ? opkt.size : 32); idx++){
			printf(" %x ",opkt.data[idx]);
		}
		printf(" ]\n");



		// Appending SPS+PPS in front of IDR frame
		if ((opkt.data[0] == 0) && (opkt.data[1] == 0) && (opkt.data[2] == 0) &&
			(opkt.data[3] == 1) && (opkt.data[4] == 0x65)) {
			ret = fwrite(sps_pps_data, 1, sps_pps_size, video_pkt_fp);
		}

		ret = fwrite(opkt.data, 1, opkt.size, video_pkt_fp);


		av_packet_unref(&opkt);



	} else if ( pkt.stream_index == audio_stream_idx) {
		printf("Write pkt_size=%d into file\n",pkt_size);

		int idx = 0;
		printf("pkt.data=[ ");
		for (idx = 0; idx < (pkt_size < 32 ? pkt_size : 32); idx++){
			printf(" %x ",pkt.data[idx]);
		}
		printf(" ]\n");

		char out_pkt_size[4] = {0};
		sprintf(out_pkt_size, "%-0d", pkt_size);


		ret = fwrite(out_pkt_size, 1, 4, audio_pkt_fp);
		ret = fwrite(pkt.data, 1, pkt_size, audio_pkt_fp);
	}


	return ret;
}


static int open_codec_context(int *stream_idx,
                              AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx, 
							  enum AVMediaType type,
							  AVCDecoderConfiguration& avc_conf)
{
    int ret, stream_index;
    AVStream *st;
    AVCodec *dec = NULL;
    AVDictionary *opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders, with or without reference counting */
        av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
        if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}


#if 1
#define MAX_BUF_SIZE 20480

static int32_t get_media_abstract_duration_time(const string ffmpeg_path, 
		const string in_path_filename, string& duration_time){
	string ffmpeg_cmd = ffmpeg_path + " -i " + in_path_filename + " -f null - 2>&1";

	FILE   *stream; 
	char   buf[MAX_BUF_SIZE]; 
	memset( buf, '\0', sizeof(buf) );        //初始化buf,以免后面写如乱码到文件中
	stream =  popen(ffmpeg_cmd.c_str(), "r");
	fread(buf, sizeof(char), sizeof(buf), stream);
	string ffmpeg_output = buf;

	printf("FFmpeg INFO  size=%d : \n%s", ffmpeg_output.size(), ffmpeg_output.c_str());
	
	string start_time;
	uint32_t pos= 0;
	if ((pos = ffmpeg_output.find("Duration:",pos+1))!=string::npos){		
		duration_time = ffmpeg_output.substr(pos+10,  11);
		printf("duration_time=%s", duration_time.c_str());
	}
	pclose(stream);

	return 0;
}
#endif

static int32_t check_media_validity(const string ffmpeg_path, const string in_path_filename, bool& valid){
	int32_t ret = 0;

	if (access(in_path_filename.c_str(),R_OK) !=0){
		printf("Access file %s failed\n", in_path_filename.c_str());
		valid = false;	
		return ret;
	}

	string ffmpeg_cmd = ffmpeg_path + " -i " + in_path_filename + " -f null - 2>&1";

	FILE   *stream; 
	char   buf[MAX_BUF_SIZE]; 
	memset( buf, '\0', sizeof(buf) );        //初始化buf,以免后面写如乱码到文件中
	stream =  popen(ffmpeg_cmd.c_str(), "r");
	fread(buf, sizeof(char), sizeof(buf), stream);
	string ffmpeg_output = buf;

	printf("FFmpeg INFO  size=%d : \n%s", ffmpeg_output.size(), ffmpeg_output.c_str());
	
	int32_t pos= 0;
	if ((pos = ffmpeg_output.find("Invalid",pos+1)) != string::npos){		
		printf("Found the string , pos=%d\n", pos);
		valid = false;
	}
	pclose(stream);

	return ret;
}




int main (int argc, char **argv)
{
#if 0	
    /* 临时测试用，用于检查一个文件的真正有效时长 */
	string ffmpeg_path = "/FFMPEG_INSTALL_PATH/lib/ffmpeg";
	string in_path_filename[10] = {};
	in_path_filename[0] = "/test/a.mp4";
	in_path_filename[1] = "/test/b.mp4";
	in_path_filename[2] = "/test/c.mp4";
	in_path_filename[3] = "/test/d.mp4";
	in_path_filename[4] = "/test/e.mp4";
	in_path_filename[5] = "/test/f.mp4";
	in_path_filename[6] = "/test/g.mp4";
	string duration_time;

    // Check file 
    int32_t iret = 0;
	bool    media_valid = true;
	int32_t idx = 0;
	for (idx = 0; idx < 10; idx++){
		media_valid = true;
		iret =  check_media_validity(ffmpeg_path, in_path_filename[idx], media_valid);
		if (iret < 0){
			printf("Failed to check media_valid\n");	
		}
		if (!media_valid){
			printf("meida file %d:%s is bad\n", idx, in_path_filename[idx].c_str());
		} else {
			printf("meida file %d:%s is good!!!\n\n\n", idx, in_path_filename[idx].c_str());
		}
	}
	return 0;
#endif
 

    int ret = 0, got_frame;

    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s [-refcount] input_file video_output_file audio_output_file\n"
                "API example program to show how to read frames from an input file.\n"
                "This program reads frames from a file, decodes them, and writes decoded\n"
                "video frames to a rawvideo file named video_output_file, and decoded\n"
                "audio frames to a rawaudio file named audio_output_file.\n\n"
                "If the -refcount option is specified, the program use the\n"
                "reference counting frame system which allows keeping a copy of\n"
                "the data for longer than one decode call.\n"
                "\n", argv[0]);
        exit(1);
    }
    if (argc == 5 && !strcmp(argv[1], "-refcount")) {
        refcount = 1;
        argv++;
    }
    src_filename = argv[1];


	/* pkt */
	int   pkt_cnt = 0;
	int   is_first_pkt = 0;

	string video_pkt_fn = argv[2]; 	
	string audio_pkt_fn = argv[3]; 	

	FILE *video_pkt_fp = NULL;
	FILE *audio_pkt_fp = NULL;
	/* END */

	/*
    ********************************************
	*
	* initialize
	*
	**********************************************
	*/


    /* open input file, and allocate format context */
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open source file %s\n", src_filename);
        exit(1);
    }

    /* retrieve stream information */
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(1);
    }




	AVCDecoderConfiguration avc_conf;
	unsigned char sps_pps_data[1024] = {0};
	int32_t       sps_pps_size = 0;




	AVRational    in_st_time_base = {0, 0};
	int64_t       sum_pause_time = 0;
	int64_t       now_time = 0;
	int64_t       pts_time = 0;
	int64_t       st_start_time;     /* time when read started */
	int64_t       ist_dts  = 0;
	st_start_time = av_gettime_relative();
	
	int64_t last_pkt_dts = 0;			


	int64_t       prev_pts_time = 0;
	int64_t       prev_sys_time = 0;


    if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO, avc_conf) >= 0) {
        video_stream = fmt_ctx->streams[video_stream_idx];
		in_st_time_base.num = video_stream->time_base.num;
		in_st_time_base.den = video_stream->time_base.den;
		printf("VIDEO: in_st_time_base.num=%d, in_st_time_base.den=%d\n", in_st_time_base.num, in_st_time_base.den);


		int i = 0; 
		printf("SPS+PPS size=%d ;  data[ ", fmt_ctx->streams[0]->codec->extradata_size);
		for (i=0; i < fmt_ctx->streams[0]->codec->extradata_size;i++){
			printf(" %x ",  fmt_ctx->streams[0]->codec->extradata[i]);
		}
		printf(" ]\n");
		fflush(NULL);

		
		ret = parse_avc_configuration(fmt_ctx->streams[0]->codec->extradata, fmt_ctx->streams[0]->codec->extradata_size, &avc_conf);
		if (ret < 0){
			printf("Error in parse_avc_configuration \n");
		}
		
		printf("SPS size=%d, SPS[ ", avc_conf.sequenceParameterSetLength);
		for (i = 0; i < avc_conf.sequenceParameterSetLength; i++){
			printf(" %x ", avc_conf.sequenceParameterSetNALUnit[i]);
		}
		printf(" ]\n");
		fflush(NULL);

		printf("PPS size=%d, PPS[ ", avc_conf.pictureParameterSetLength);
		for (i = 0; i < avc_conf.pictureParameterSetLength; i++){
			printf(" %x ", avc_conf.pictureParameterSetNALUnit[i]);
		}
		printf(" ]\n");
		fflush(NULL);


		memcpy(sps_pps_data, avc_conf.sequenceParameterSetNALUnit, avc_conf.sequenceParameterSetLength);
		memcpy(&sps_pps_data[avc_conf.sequenceParameterSetLength], avc_conf.pictureParameterSetNALUnit, avc_conf.pictureParameterSetLength);
		sps_pps_size = avc_conf.sequenceParameterSetLength + avc_conf.pictureParameterSetLength;



		/* pkt */
		video_pkt_fp  = fopen(video_pkt_fn.c_str(), "wb");
		if (!video_pkt_fp) {
           fprintf(stderr, "Could not open destination file %s\n", video_pkt_fn.c_str());
           ret = 1;
           goto end;
        }
		/* END */	
    }

    if (open_codec_context(&audio_stream_idx, &audio_dec_ctx, fmt_ctx, AVMEDIA_TYPE_AUDIO, avc_conf) >= 0) {
        audio_stream = fmt_ctx->streams[audio_stream_idx];
		in_st_time_base.num = audio_stream->time_base.num;
		in_st_time_base.den = audio_stream->time_base.den;
		printf("AUDIO: in_st_time_base.num=%d, in_st_time_base.den=%d\n", in_st_time_base.num, in_st_time_base.den);

		/* pkt */	
        audio_pkt_fp = fopen(audio_pkt_fn.c_str(), "wb");
        if (!audio_pkt_fp) {
            fprintf(stderr, "Could not open destination file %s\n", audio_pkt_fn.c_str());
            ret = 1;
            goto end;
        }
		/* END */
    }

    /* dump input information to stderr */
    av_dump_format(fmt_ctx, 0, src_filename, 0);

    if (!audio_stream && !video_stream) {
        fprintf(stderr, "Could not find audio or video stream in the input, aborting\n");
        ret = 1;
        goto end;
    }

	/*
    ********************************************
	*
	* loop
	*
	**********************************************
	*/

    /* initialize packet, set data to NULL, let the demuxer fill it */
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
	pkt.dts  =  AV_NOPTS_VALUE;
	pkt.pts  =  AV_NOPTS_VALUE;

    if (video_stream)
        printf("Demuxing video from file '%s' into '%s'\n", src_filename, video_pkt_fn.c_str());
    if (audio_stream)
        printf("Demuxing audio from file '%s' into '%s'\n", src_filename, audio_pkt_fn.c_str());




	
    /* read frames from the file */
    while (1) {

		printf("\n\n*********************** pkt_cnt=%d ********************* \n",
				pkt_cnt);

		printf("pkt.dts=%lld\n",  pkt.dts);
	

		ist_dts = av_rescale_q(pkt.dts, in_st_time_base, AV_TIME_BASE_Q);
		pts_time = av_rescale(ist_dts, 1000000, AV_TIME_BASE);
		now_time = av_gettime_relative() - st_start_time;


		if ((pkt.dts != AV_NOPTS_VALUE) && (pts_time > now_time)){
			av_usleep(1000);
			continue;
		}

		
		printf("pts_time(=%lld) = av_rescale(pkt.dts=%lld), now_time(=%lld) = now- start(%lld)\n", 
			pts_time, pkt.dts,  now_time, st_start_time);


		printf("diff_pts_time=%lld, diff_sys_time=%lld\n", (pts_time-prev_pts_time), (now_time-prev_sys_time));
		prev_pts_time=pts_time, prev_sys_time=now_time;


		// demuxing	
		ret = av_read_frame(fmt_ctx, &pkt);
		if (ret < 0){
			break;
		}
		last_pkt_dts = pkt.dts;
		

		// saving
		if (pkt_cnt == 0){
			is_first_pkt = 1;
		} else {
			is_first_pkt = 0;
		}
		ret = save_packet(fmt_ctx, pkt, sps_pps_data, sps_pps_size, video_pkt_fp, audio_pkt_fp, is_first_pkt);
		if (ret < 0){
			break;
		}




		av_packet_unref(&pkt);	
		pkt.dts = last_pkt_dts;
		/*
		if ( pkt_cnt > 600) 
		{
			break;
		}
		*/
		
		pkt_cnt++;
    }


	/*
    ********************************************
	*
	* flush
	*
	**********************************************
	*/
	av_packet_unref(&pkt);	

    printf("Demuxing succeeded.\n");

	/*
    ********************************************
	*
	* release
	*
	**********************************************
	*/
end:
    avcodec_free_context(&video_dec_ctx);
    avcodec_free_context(&audio_dec_ctx);
    avformat_close_input(&fmt_ctx);

    if (video_pkt_fp)
        fclose(video_pkt_fp);
    if (audio_pkt_fp)
        fclose(audio_pkt_fp);

    return ret < 0;
}
