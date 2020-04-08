#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include<iostream>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "strmif.h"
#include <initguid.h>
#include<vector>
#include"EasyWay.h"
extern "C"
{
#include "SDL2/SDL.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include<libavutil/frame.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include "libavdevice/avdevice.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavdevice/avdevice.h"
#include "libavutil/imgutils.h"
}

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000
//#define MAX_AUDIO_FRAME_SIZE 4096



typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;
struct SwrContext *audio_convert_ctx;

int quit = 0;
int out_buffer_size = -1;

void packet_queue_init(PacketQueue *q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
	AVPacketList *pkt1;
	//引用计数加1
	if (av_packet_make_refcounted(pkt) < 0) {
		return -1;
	}
	
	pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;

	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);

	if (!q->last_pkt)    //队列为空
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;  //将当前链表的最后一个结点的next指向pkt1

	q->last_pkt = pkt1; //将last_pkt指向最后一个结点，即pkt1
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
	return 0;
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for (;;)
	{
		if (quit)
		{
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt; //指向第一个结点，即取出第一个结点
		if (pkt1)
		{
			q->first_pkt = pkt1->next; //即q->first_pkt = q->first_pkt->next 将第一个结点指向的下一个结点设置为first_pkt，可以理解为取出当前第一个结点

			if (!q->first_pkt)
				q->last_pkt = NULL;

			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block)
		{
			ret = 0;
			break;
		}
		else
		{
			SDL_CondWait(q->cond, q->mutex);
		}
	}

	SDL_UnlockMutex(q->mutex);
	return ret;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf) {
	AVPacket pkt;
	AVFrame *frame = av_frame_alloc();
	static int packetFinish = 1;
	int data_size = 0;
	int ret;
	if (packetFinish) {
		if (packet_queue_get(&audioq, &pkt, 1) < 0) {
			return -1;
		}
		ret = avcodec_send_packet(aCodecCtx, &pkt);

		if (ret != 0) {
			printf("avcodec_send_packet fail\n");
			return -1;
		}
		av_packet_unref(&pkt);
	}


	ret = avcodec_receive_frame(aCodecCtx, frame);
	if (ret != 0) {
		printf("error or packet eof, skip");
		packetFinish = 1;
		audio_decode_frame(aCodecCtx, audio_buf);
	}
	int len = swr_convert(audio_convert_ctx, &audio_buf, MAX_AUDIO_FRAME_SIZE, (const uint8_t**)&frame->data[0], frame->nb_samples);
	data_size = out_buffer_size;
	data_size = 2 * len*av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
	av_frame_free(&frame);
	return data_size;
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
	
	AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
	int len1, audio_size;
	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	//读取的数据长度
	static unsigned int audio_buf_size = 0;
	//尚未使用的数据的开始索引
	static unsigned int audio_buf_index = 0;
	//audio_size = audio_decode_frame(aCodecCtx, audio_buf);
	//SDL_MixAudio(stream, audio_buf, len, SDL_MIX_MAXVOLUME);
	while (len > 0)
	{
		if (audio_buf_index >= audio_buf_size) {
			audio_size = audio_decode_frame(aCodecCtx, audio_buf);
			if (audio_size < 0) {
				// If error, output silence.
				audio_buf_size = 1024;
				memset(audio_buf, 0, audio_buf_size);
			}
			else {
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}
		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len) {
			len1 = len;
		}
		printf("%d\n", *(audio_buf + audio_buf_index));
		//SDL_MixAudio(stream, audio_buf + audio_buf_index, len1, SDL_MIX_MAXVOLUME);
		memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
		len -= len1;
		
		stream += len1;
		audio_buf_index += len1;
	}

	//memset(stream, 125, len);


}

int playAudio() {
	AVFormatContext *pFormatCtx = NULL;
	int audioIndex = -1;
	AVStream *audioStream = NULL;
	AVCodecContext *codecCtx;
	AVCodec *codec;

	SDL_AudioSpec wanted_spec, spec{};
	AVPacket *pPkt = av_packet_alloc();
	av_init_packet(pPkt);
	avdevice_register_all();
	audio_convert_ctx = swr_alloc();

	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}

	packet_queue_init(&audioq);

	pFormatCtx = avformat_alloc_context();

	if (avformat_open_input(&pFormatCtx, "helloteacher.avi", NULL, NULL) != 0) {
		printf("could not find video\n");
		exit(-1);
	}

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
		printf("could not find stream info");
		exit(-1);
	}

	av_dump_format(pFormatCtx, 0, "helloteacher.avi", 0);

	for (int i = 0; i < pFormatCtx->nb_streams; ++i) {
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audioIndex = i;
			audioStream = pFormatCtx->streams[i];
		}
	}

	printf("audeo stream: %d\n", audioIndex);

	if (audioIndex == -1) {
		printf("Didn't find a audio stream.\n");
		return -1;
	}

	codec = avcodec_find_decoder(audioStream->codecpar->codec_id);
	if (!codec) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	codecCtx = avcodec_alloc_context3(codec);
	
	if ((avcodec_parameters_to_context(codecCtx, audioStream->codecpar)) < 0) {
		fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
			av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
		return -1;
	}

	//aCodecCtx = pFormatCtx->streams[audioStream]->codec;
	

	if (avcodec_open2(codecCtx, codec, NULL) != 0) {
		printf("codecopen fail\n");
		exit(-1);
	}

	uint64_t out_chn_layout = AV_CH_LAYOUT_STEREO;  //通道布局 输出双声道
	enum AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16; //声音格式
	int out_sample_rate = 44100;   //采样率
	int out_nb_samples = -1;
	int out_channels = -1;        //通道数

	uint64_t in_chn_layout = -1;  //通道布局 

	out_nb_samples = codecCtx->frame_size;
	out_channels = av_get_channel_layout_nb_channels(out_chn_layout);
	out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);
	printf("-------->out_buffer_size is %d\n", out_buffer_size);
	in_chn_layout = codecCtx->channel_layout;

	// Set audio settings from codec info.
	/*wanted_spec.freq = out_sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = out_channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = codecCtx;*/
	wanted_spec.freq = codecCtx->sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = codecCtx->channels;
	wanted_spec.samples = 1024; //set by output samples
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = codecCtx;

	audio_convert_ctx = swr_alloc_set_opts(NULL,
		out_chn_layout,                                /*out*/
		//av_get_default_channel_layout(spec.channels),
		out_sample_fmt,                              /*out*/
		//out_sample_rate,                             /*out*/
		codecCtx->sample_rate,
		in_chn_layout,                                  /*in*/
		codecCtx->sample_fmt,               /*in*/
		codecCtx->sample_rate,               /*in*/
		0,
		NULL);

	swr_init(audio_convert_ctx);

	SDL_AudioDeviceID audioDeviceID;
	audioDeviceID = SDL_OpenAudioDevice(  // [1]
		nullptr, 0, &wanted_spec, &spec, 0);
	if (audioDeviceID == 0) {
		string errMsg = "Failed to open audio device:";
		errMsg += SDL_GetError();
		cout << errMsg << endl;
		throw std::runtime_error(errMsg);
	}


	
	
	
	

	cout << "wanted_specs.freq:" << wanted_spec.freq << endl;
	// cout << "wanted_specs.format:" << wanted_specs.format << endl;
	std::printf("wanted_specs.format: Ox%X\n", wanted_spec.format);
	cout << "wanted_specs.channels:" << (int)wanted_spec.channels << endl;
	cout << "wanted_specs.samples:" << (int)wanted_spec.samples << endl;

	cout << "------------------------------------------------" << endl;

	cout << "specs.freq:" << spec.freq << endl;
	// cout << "specs.format:" << specs.format << endl;
	std::printf("specs.format: Ox%X\n", spec.format);
	cout << "specs.channels:" << (int)spec.channels << endl;
	cout << "specs.silence:" << (int)spec.silence << endl;
	cout << "specs.samples:" << (int)spec.samples << endl;

	SDL_PauseAudioDevice(audioDeviceID, 0);
	while (av_read_frame(pFormatCtx, pPkt) >= 0)
	{
		if (pPkt->stream_index == audioIndex) {
			packet_queue_put(&audioq, pPkt);
		}
		else {
			av_packet_unref(pPkt);
		}
	}
	SDL_Delay(300000);
	SDL_CloseAudio();
	SDL_Quit();

	swr_free(&audio_convert_ctx);

	avcodec_close(codecCtx);
	avformat_close_input(&pFormatCtx);

	return 0;
}