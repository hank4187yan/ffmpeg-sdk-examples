/**
 * 最简单的基于FFmpeg的内存读写例子（内存转码器）
 * Simplest FFmpeg mem Transcoder
 *
 * 本程序实现了任意格式视频数据（例如MPEG2）转码为H.264码流数据。
 * 本程序并不是对文件进行处理，而是对内存中的视频数据进行处理。
 * 它从内存读取数据，并且将转码后的数据输出到内存中。
 * 是最简单的使用FFmpeg读写内存的例子。
 *
 * This software convert video bitstream (Such as MPEG2) to H.264
 * bitstream. It read video bitstream from memory (not from a file),
 * convert it to H.264 bitstream, and finally output to another memory.
 * It's the simplest example to use FFmpeg to read (or write) from 
 * memory.
 *
 */

#include <stdio.h>


#ifdef __cplusplus
extern "C" {
#endif
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#ifdef __cplusplus
}
#endif

 
static FILE *fp_open;
static FILE *fp_write;

static AVCodecContext **dec_ctx_list;
static AVCodecContext **enc_ctx_list;;
 
/* Read File */
int read_buffer(void *opaque, uint8_t *buf, int buf_size){
	printf("read_buffer(), buf_size=%d\n", buf_size);

	if(!feof(fp_open)){
		int true_size=fread(buf,1,buf_size,fp_open);
		return true_size;
	}else{
		return -1;
	}
}
 


/* Write File */
int write_buffer(void *opaque, uint8_t *buf, int buf_size){
	printf("write_buffer(), buf_size=%d\n", buf_size);

	if(!feof(fp_write)){
		int true_size=fwrite(buf,1,buf_size,fp_write);
		return true_size;
	}else{
		return -1;
	}
}
 
 
 
int flush_encoder(AVFormatContext *fmt_ctx,unsigned int stream_index)
{
    int ret = 0;
    int got_frame;
	AVPacket enc_pkt;
    
	AVCodecParameters *av_codec_par = fmt_ctx->streams[stream_index]->codecpar; 
	if (!(av_codec_par->codec_type & AV_CODEC_CAP_DELAY))
        return 0;

    while (1) {
        av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);
        enc_pkt.data = NULL;
		enc_pkt.size = 0;
		av_init_packet(&enc_pkt);

		AVCodecContext* av_codec_ctx =  enc_ctx_list[stream_index];

		avcodec_send_frame(av_codec_ctx, NULL);
		got_frame = avcodec_receive_packet(av_codec_ctx,  &enc_pkt);



		av_frame_free(NULL);
		if (ret < 0)
			break;
		if (got_frame != 0){
			ret=0;
			break;
		}

		/* prepare packet for muxing */
		enc_pkt.stream_index = stream_index;

		enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,
				                       av_codec_ctx->time_base,
									   fmt_ctx->streams[stream_index]->time_base,
									   (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));



		enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,
									   av_codec_ctx->time_base,
									   fmt_ctx->streams[stream_index]->time_base,
									   (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));

		enc_pkt.duration = av_rescale_q(enc_pkt.duration,
										av_codec_ctx->time_base,
										fmt_ctx->streams[stream_index]->time_base);

		/* mux encoded frame */
		av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
		ret = av_write_frame(fmt_ctx, &enc_pkt);
		if (ret < 0)
            break;
    }
    return ret;
}
 
 

int main(int argc, char* argv[])
{
	int ret;
	AVFormatContext* ifmt_ctx=NULL;
	AVFormatContext* ofmt_ctx=NULL;
	AVPacket packet,enc_pkt;
	AVFrame *frame = NULL;
	unsigned int stream_index;
	unsigned int i=0;
	int got_frame;
 
	AVStream *out_stream;



	dec_ctx_list = new AVCodecContext*[20];
	enc_ctx_list = new AVCodecContext*[20];
	for (int i=0; i < 20; i++){
		dec_ctx_list[i] = new AVCodecContext;
		enc_ctx_list[i] = new AVCodecContext;
	}

	AVCodecContext *dec_ctx, *enc_ctx;
	AVCodec *encoder;



 
	fp_open = fopen("vipkid_5592167_f_1554551997481_t_1554553671031.ts", "rb");	//视频源文件 
	fp_write=fopen("vipkid_5592167_f_1554551997481_t_1554553671031.ts.h264","wb+"); //输出文件
 
	ifmt_ctx = avformat_alloc_context();
	//avformat_alloc_output_context2(&ofmt_ctx, NULL, "h264", NULL);
	ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, "h264", NULL);
	if (ret < 0){
		av_log(NULL, AV_LOG_ERROR, "Failed to allocat output context\n");
		return -1;
	}
 


	unsigned char* inbuffer=NULL;
	unsigned char* outbuffer=NULL;
	inbuffer=(unsigned char*)av_malloc(32768);
	outbuffer=(unsigned char*)av_malloc(32768);

	AVIOContext *avio_in  = NULL;
	AVIOContext *avio_out  = NULL;



	/* Register read callback function */
	avio_in =avio_alloc_context(inbuffer, 32768,0,NULL,read_buffer,NULL,NULL);  
	if(avio_in==NULL)
		goto end;
	ifmt_ctx->pb=avio_in; 
	ifmt_ctx->flags=AVFMT_FLAG_CUSTOM_IO;



	/*open input file*/
	if ((ret = avformat_open_input(&ifmt_ctx, "whatever", NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
		return ret;
	}
	if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
		return ret;
	}

    av_dump_format(ifmt_ctx, 0, NULL, 0);



	/* open  decoder */
	for (i = 0; i < ifmt_ctx->nb_streams; i++) {
		AVCodecContext *codec_ctx;

		codec_ctx = avcodec_alloc_context3(NULL);
		avcodec_parameters_to_context(codec_ctx, ifmt_ctx->streams[i]->codecpar);


		/* Reencode video & audio and remux subtitles etc. */
		if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO){
			/* Open decoder */
			ret = avcodec_open2(codec_ctx, avcodec_find_decoder(codec_ctx->codec_id), NULL);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
				return ret;
			}
		}
		dec_ctx_list[i] = codec_ctx;
	}
	av_dump_format(ifmt_ctx, 0, "whatever", 0);
 


	/* Register ouput callback function */
	avio_out =avio_alloc_context(outbuffer, 32768,1,NULL,NULL,write_buffer,NULL);  
	if(avio_out==NULL)
		goto end;
	ofmt_ctx->pb=avio_out; 
	ofmt_ctx->flags=AVFMT_FLAG_CUSTOM_IO;




	/*open output file*/
	for (i = 0; i < 1; i++) {
		out_stream = avformat_new_stream(ofmt_ctx, NULL);
		if (!out_stream) {
			av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
			return AVERROR_UNKNOWN;
		}

		dec_ctx = dec_ctx_list[i];

		enc_ctx = avcodec_alloc_context3(NULL);
		avcodec_parameters_to_context(enc_ctx, out_stream->codecpar);




		/* open encoder */
		if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
		{

			encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
			enc_ctx->height  = dec_ctx->height;
			enc_ctx->width   = dec_ctx->width;
			enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
			enc_ctx->pix_fmt = encoder->pix_fmts[0];

			//H264的必备选项，没有就会错
			enc_ctx->time_base.num = 1;
			enc_ctx->time_base.den = 25;

			enc_ctx->me_range=16;
			enc_ctx->max_qdiff = 4;
			enc_ctx->qmin = 10;
			enc_ctx->qmax = 51;
			enc_ctx->qcompress = 0.6; 
			enc_ctx->refs=3;
			enc_ctx->bit_rate = 500000;
 
			ret = avcodec_open2(enc_ctx, encoder, NULL);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", i);
				return ret;
			}
		}
		else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
			av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
			return AVERROR_INVALIDDATA;
		} else {
			/* if this stream must be remuxed */	
			ret = avcodec_copy_context(enc_ctx, dec_ctx);
			if (ret < 0) {
				av_log(NULL, AV_LOG_ERROR, "Copying stream context failed\n");
				return ret;
			}
			dec_ctx->coded_side_data = NULL;
			dec_ctx->nb_coded_side_data = 0;

		}


		if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
			enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;


		enc_ctx_list[i] = enc_ctx;
	}
	av_dump_format(ofmt_ctx, 0, "whatever", 1);

		

	/* init muxer, write output file header */
	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
		return ret;
	}



 
	i=0;
	/* read all packets */
	while (1) {
		i++;
		av_log(NULL, AV_LOG_INFO, "Going to reencode the frame\n");


		/* read packet */
		if ((ret = av_read_frame(ifmt_ctx, &packet)) < 0){
			break;
		}

		stream_index = packet.stream_index;
		if(stream_index!=0)
			continue;
 
		frame = av_frame_alloc();
		if (!frame) {
			ret = AVERROR(ENOMEM);
			break;
		}

		packet.dts = av_rescale_q_rnd(packet.dts,
			ifmt_ctx->streams[stream_index]->time_base,
			dec_ctx->time_base, //ifmt_ctx->streams[stream_index]->codec->time_base,
			(AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));

		packet.pts = av_rescale_q_rnd(packet.pts,
			ifmt_ctx->streams[stream_index]->time_base,
			dec_ctx->time_base, //ifmt_ctx->streams[stream_index]->codec->time_base,
			(AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));


		/* decode */
		ret = avcodec_send_packet(dec_ctx, &packet);
		got_frame = avcodec_receive_frame(dec_ctx, frame);

		printf("Decode 1 Packet size:%d  pts:%"PRId64"\n", packet.size, packet.pts);
 
		if (ret < 0) {
			av_frame_free(&frame);
			av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
			break;
		}


	
		/* encode */
		if (got_frame == 0) {
			frame->pts = av_frame_get_best_effort_timestamp(frame);
			frame->pict_type=AV_PICTURE_TYPE_NONE;
 
			enc_pkt.data = NULL;
			enc_pkt.size = 0;
			av_init_packet(&enc_pkt);

			ret = avcodec_send_frame(enc_ctx, frame);   
			ret = avcodec_receive_packet(enc_ctx, &enc_pkt);
			if ( ret == 0){
				printf("Encode 1 Packet\tsize:%d\tpts:%"PRId64"\n",enc_pkt.size,enc_pkt.pts);
			} else {
				continue;
			}
			
 
 
			av_frame_free(&frame);
			if (ret < 0)
				goto end;

			/* prepare packet for muxing */
			enc_pkt.stream_index = stream_index;
			enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,
				dec_ctx->time_base, //ofmt_ctx->streams[stream_index]->codec->time_base,
				ofmt_ctx->streams[stream_index]->time_base,
				(AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
			enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,
				dec_ctx->time_base, //ofmt_ctx->streams[stream_index]->codec->time_base,
				ofmt_ctx->streams[stream_index]->time_base,
				(AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
			enc_pkt.duration = av_rescale_q(enc_pkt.duration,
				dec_ctx->time_base, //ofmt_ctx->streams[stream_index]->codec->time_base,
				ofmt_ctx->streams[stream_index]->time_base);
			av_log(NULL, AV_LOG_INFO, "Muxing frame %d\n",i);


			/* mux encoded frame */
			av_write_frame(ofmt_ctx,&enc_pkt);
			if (ret < 0)
				goto end;
		} else {
			av_frame_free(&frame);
		}
 
		//av_free_packet(&packet);
		av_packet_unref(&packet);
	}
 



	/* flush encoders */
	for (i = 0; i < 1; i++) {
		/* flush encoder */
		ret = flush_encoder(ofmt_ctx,i);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
			goto end;
		}
	}
	av_write_trailer(ofmt_ctx);

end:
	av_freep(avio_in);
	av_freep(avio_out);
	av_free(inbuffer);
	av_free(outbuffer);
	av_packet_unref(&packet);

	av_frame_free(&frame);
	avformat_close_input(&ifmt_ctx);
	avformat_free_context(ofmt_ctx);
 
 
	if (ret < 0)
		av_log(NULL, AV_LOG_ERROR, "Error occurred\n");
	return (ret? 1:0);
}
