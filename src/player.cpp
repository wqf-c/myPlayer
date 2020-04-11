#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include<iostream>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "strmif.h"
#include <initguid.h>
#include <string>
#include<thread>
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

using std::string;
using std::cout;
using std::endl;
using std::thread;

typedef struct PacketQueue {
	AVPacketList			*first_pkt, *last_pkt;
	int						nb_packets;
	int						size;
	SDL_mutex				*mutex;
	SDL_cond				*cond;
	long					currentPlayTime;
	AVRational				streamTimeBase;
} PacketQueue;

typedef struct FileInfo {
	AVFormatContext			*pFormatCtx;
	AVStream				*audioStream;
	int						audioIndex;
	AVStream				*videoStream;
	int						videoIndex;
	AVCodec					*audioCodec;
	AVCodec					*videoCodec;
	AVCodecContext			*audioCodecCtx;
	AVCodecContext			*videoCodecCtx;
	struct SwrContext		*audio_convert_ctx;
	struct SwsContext		*img_convert_ctx;
	string					fileName;
	bool					playVideo;
	bool					playAudio;
	bool					fileEnd;
	AVFrame					*pFrameYUV;
	AVFrame					*pFrame;
	boolean					faster;
} FileInfo;

PacketQueue audioQ, videoQ;
FileInfo fileInfo{NULL, NULL, -1, NULL, -1, NULL, NULL, NULL, NULL, NULL, NULL, "", true, true, false, false};

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)

#define SFM_BREAK_EVENT  (SDL_USEREVENT + 2)


int					quit = 0;
int					out_buffer_size = -1;
int					video_thread_exit = 0;

int getFrameRate(AVStream * inputVideoStream) {
	int frameRate = 40;
	if (inputVideoStream != nullptr && inputVideoStream->r_frame_rate.den > 0)
	{
		frameRate = inputVideoStream->r_frame_rate.num / inputVideoStream->r_frame_rate.den;
	}
	else if (inputVideoStream != nullptr && inputVideoStream->r_frame_rate.den > 0)
	{

		frameRate = inputVideoStream->r_frame_rate.num / inputVideoStream->r_frame_rate.den;
	}
	return frameRate;
}

int sfp_refresh_thread(int time) {
	video_thread_exit = 0;
	cout << "time:" <<  time << endl;
	while (!video_thread_exit) {
		SDL_Event event;
		event.type = SFM_REFRESH_EVENT;
		//cout << "send refresh event" << endl;
		SDL_PushEvent(&event);
		int delay = fileInfo.faster ? time / 2 : time;
		SDL_Delay(delay);
		if (fileInfo.fileEnd && videoQ.nb_packets <= 0) {
			break;
		}
	}
	video_thread_exit = 0;
	//Break
	SDL_Event event;
	event.type = SFM_BREAK_EVENT;
	SDL_PushEvent(&event);

	return 0;
}

void packet_queue_init() {
	memset(&audioQ, 0, sizeof(PacketQueue));
	audioQ.mutex = SDL_CreateMutex();
	audioQ.cond = SDL_CreateCond();
	memset(&videoQ, 0, sizeof(PacketQueue));
	videoQ.mutex = SDL_CreateMutex();
	videoQ.cond = SDL_CreateCond();
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

int initFileInfo(string fileName) {
	fileInfo.fileName = fileName;
	fileInfo.pFormatCtx = avformat_alloc_context();

	if (avformat_open_input(&(fileInfo.pFormatCtx), fileName.c_str(), NULL, NULL) != 0) {
		printf("could not find video\n");
		fileInfo.playAudio = false;
		fileInfo.playVideo = false;
		return -1;
	}

	if (avformat_find_stream_info(fileInfo.pFormatCtx, NULL) < 0) {
		printf("could not find stream info");
		fileInfo.playAudio = false;
		fileInfo.playVideo = false;
		return -1;
	}

	av_dump_format(fileInfo.pFormatCtx, 0, fileName.c_str(), 0);

	for (int i = 0; i < fileInfo.pFormatCtx->nb_streams; ++i) {
		if (fileInfo.pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			fileInfo.audioStream = fileInfo.pFormatCtx->streams[i];
			fileInfo.audioIndex = i;
		}
		if (fileInfo.pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			fileInfo.videoStream = fileInfo.pFormatCtx->streams[i];
			fileInfo.videoIndex = i;
		}
	}
	if (fileInfo.audioStream != NULL) {
		while (true)
		{
			fileInfo.audioCodec = avcodec_find_decoder(fileInfo.audioStream->codecpar->codec_id);
			fileInfo.audioCodecCtx = avcodec_alloc_context3(fileInfo.audioCodec);

			if ((avcodec_parameters_to_context(fileInfo.audioCodecCtx, fileInfo.audioStream->codecpar)) < 0) {
				fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
					av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
				fileInfo.playAudio = false;
				break;
			}

			//aCodecCtx = pFormatCtx->streams[audioStream]->codec;


			if (avcodec_open2(fileInfo.audioCodecCtx, fileInfo.audioCodec, NULL) != 0) {
				printf("audio codec open fail\n");
				fileInfo.playAudio = false;
				break;
			}

			fileInfo.audio_convert_ctx = swr_alloc();
			fileInfo.audio_convert_ctx = swr_alloc_set_opts(NULL,
				AV_CH_LAYOUT_STEREO,                                /*out*/
				//av_get_default_channel_layout(spec.channels),
				AV_SAMPLE_FMT_S16,                              /*out*/
				//out_sample_rate,                             /*out*/
				fileInfo.audioCodecCtx->sample_rate,
				fileInfo.audioCodecCtx->channel_layout,                                  /*in*/
				fileInfo.audioCodecCtx->sample_fmt,               /*in*/
				fileInfo.audioCodecCtx->sample_rate,               /*in*/
				0,
				NULL);

			swr_init(fileInfo.audio_convert_ctx);
			out_buffer_size = av_samples_get_buffer_size(NULL, av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO),
				fileInfo.audioCodecCtx->frame_size, AV_SAMPLE_FMT_S16, 1);
			break;
		}
		
	}
	else {
		cout << "no audio stream" << endl;
		fileInfo.playAudio = false;
	}

	if (fileInfo.videoStream != NULL) {
		while (true)
		{
			fileInfo.videoCodec = avcodec_find_decoder(fileInfo.videoStream->codecpar->codec_id);
			fileInfo.videoCodecCtx = avcodec_alloc_context3(NULL);

			if ((avcodec_parameters_to_context(fileInfo.videoCodecCtx, fileInfo.videoStream->codecpar)) < 0) {
				fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
					av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
				fileInfo.playVideo = false;
				break;
			}

			//aCodecCtx = pFormatCtx->streams[audioStream]->codec;
			int ret = avcodec_open2(fileInfo.videoCodecCtx, fileInfo.videoCodec, NULL);

			if (ret != 0) {
				printf("ret:%d\n", ret);
				printf("video codec open fail\n");
				fileInfo.playVideo = false;
				break;
			}
			fileInfo.pFrame = av_frame_alloc();
			fileInfo.pFrameYUV = av_frame_alloc();
			uint8_t *out_buffer = (uint8_t *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 
				fileInfo.videoCodecCtx->width, fileInfo.videoCodecCtx->height, 32) * sizeof(uint8_t));

			av_image_fill_arrays(fileInfo.pFrameYUV->data, fileInfo.pFrameYUV->linesize, 
				out_buffer, AV_PIX_FMT_YUV420P, fileInfo.videoCodecCtx->width, fileInfo.videoCodecCtx->height, 32);

			fileInfo.img_convert_ctx = sws_getContext(fileInfo.videoCodecCtx->width, fileInfo.videoCodecCtx->height,
				fileInfo.videoCodecCtx->pix_fmt, fileInfo.videoCodecCtx->width, fileInfo.videoCodecCtx->height, 
				AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
			break;
		}
	}
	else {
		cout << "no video stream" << endl;
		fileInfo.playVideo = false;
	}
}

void grabFunc() {
	AVPacket *pPkt = av_packet_alloc();
	av_init_packet(pPkt);
	static int packetCount = 0;
	while (true)
	{
		int ret = av_read_frame(fileInfo.pFormatCtx, pPkt);
		if (ret >= 0) {
			if (pPkt->stream_index == fileInfo.audioIndex) {
				packet_queue_put(&audioQ, pPkt);
			}
			else if (pPkt->stream_index == fileInfo.videoIndex) {
				packet_queue_put(&videoQ, pPkt);
			}
			else {
				av_packet_unref(pPkt);
			}
			packetCount++;
			//cout << "read pkt:" << packetCount << endl;
		}
		else {
			cout << "read file end or fail:" << ret << endl;
			break;
		}
		EasyWay::sleep(10);
	}

	cout << "file read end" << endl;
	fileInfo.fileEnd = true;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf) {
	AVPacket pkt;
	AVFrame *frame = av_frame_alloc();
	static int packetFinish = 1;
	int data_size = 0;
	int ret;
	if (packetFinish) {
		if (packet_queue_get(&audioQ, &pkt, 1) < 0) {
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
	audioQ.currentPlayTime = frame->pts * av_q2d(fileInfo.audioStream->time_base) * 1000;
	int len = swr_convert(fileInfo.audio_convert_ctx, &audio_buf, MAX_AUDIO_FRAME_SIZE, 
		(const uint8_t**)&frame->data[0], frame->nb_samples);
	data_size = out_buffer_size;
	data_size = 2 * len*av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
	av_frame_free(&frame);
	return data_size;
}

int video_decode_frame() {
	AVPacket pkt;
	if (packet_queue_get(&videoQ, &pkt, 1) < 0) {
		return -1;
	}
	int ret = avcodec_send_packet(fileInfo.videoCodecCtx, &pkt);

	if (ret != 0) {
		cout << "video avcodec_send_packet fail:" << ret << endl;
		//prinitf("%s/n", "error"); 
		return -1;
	}
	ret = avcodec_receive_frame(fileInfo.videoCodecCtx, fileInfo.pFrame);
	if (ret != 0) {
		cout << "video avcodec_receive_frame fail:" << ret << endl;
		return -1;
	}
	videoQ.currentPlayTime = fileInfo.pFrame->pts * av_q2d(fileInfo.videoStream->time_base) * 1000;
	sws_scale(fileInfo.img_convert_ctx, (const uint8_t* const*)fileInfo.pFrame->data,
		fileInfo.pFrame->linesize, 0, fileInfo.videoCodecCtx->height, fileInfo.pFrameYUV->data, fileInfo.pFrameYUV->linesize);
	av_packet_unref(&pkt);

	return 0;
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
	if (fileInfo.fileEnd && audioQ.nb_packets <= 0) {
		memset(stream, 0, len);
	}
	else {
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
			//printf("%d\n", *(audio_buf + audio_buf_index));
			//SDL_MixAudio(stream, audio_buf + audio_buf_index, len1, SDL_MIX_MAXVOLUME);
			memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
			len -= len1;

			stream += len1;
			audio_buf_index += len1;
		}
	}
	

	//memset(stream, 125, len);
}

void videoThreadFunc() {
	if (fileInfo.playVideo) {
		int screen_w, screen_h;
		SDL_Window *screen;
		SDL_Renderer* sdlRenderer;
		SDL_Texture* sdlTexture;
		SDL_Rect sdlRect;
		SDL_Event event;
		screen_w = fileInfo.videoCodecCtx->width / 2;
		screen_h = fileInfo.videoCodecCtx->height / 2;
		screen = SDL_CreateWindow("player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			screen_w, screen_h, SDL_WINDOW_OPENGL);

		if (!screen) {
			printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
			return;
		}
		sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
		//IYUV: Y + U + V  (3 planes)
		//YV12: Y + V + U  (3 planes)
		sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV,
			SDL_TEXTUREACCESS_STREAMING, fileInfo.videoCodecCtx->width, fileInfo.videoCodecCtx->height);

		sdlRect.x = 0;
		sdlRect.y = 0;
		sdlRect.w = screen_w;
		sdlRect.h = screen_h;
		cout << "framerate:" << getFrameRate(fileInfo.videoStream) << endl;
		std::thread refreshThread{ sfp_refresh_thread, (int)(1000 / getFrameRate(fileInfo.videoStream)) };
		while (true)
		{
			SDL_WaitEvent(&event);
			switch (event.type)
			{
			case SFM_REFRESH_EVENT: {
				//cout << "refresh" << endl;
				if (videoQ.nb_packets > 0 || !fileInfo.fileEnd) {
					video_decode_frame();
				}
				if (videoQ.currentPlayTime < audioQ.currentPlayTime && audioQ.currentPlayTime - videoQ.currentPlayTime > 30) {
					fileInfo.faster = true;
				}
				else {
					fileInfo.faster = false;
				}
				SDL_UpdateTexture(sdlTexture, NULL, fileInfo.pFrameYUV->data[0], fileInfo.pFrameYUV->linesize[0]);
				SDL_RenderClear(sdlRenderer);
				//SDL_RenderCopy( sdlRenderer, sdlTexture, &sdlRect, &sdlRect );  
				SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
				SDL_RenderPresent(sdlRenderer);
				//SDL End-----------------------
			}break;
			case SDL_QUIT: {
				video_thread_exit = 1;
			}break;
			case SFM_BREAK_EVENT: {
				cout << "SFM_BREAK_EVENT" << endl;
			}break;

			default:
				break;
			}
		}
	}
}

void audioThreadFunc() {
	if (fileInfo.playAudio) {
		SDL_AudioSpec wanted_spec, spec{};
		wanted_spec.freq = fileInfo.audioCodecCtx->sample_rate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = fileInfo.audioCodecCtx->channels;
		wanted_spec.samples = 1024; //set by output samples
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = fileInfo.audioCodecCtx;

		SDL_AudioDeviceID audioDeviceID;
		audioDeviceID = SDL_OpenAudioDevice(  // [1]
			nullptr, 0, &wanted_spec, &spec, 0);
		if (audioDeviceID == 0) {
			string errMsg = "Failed to open audio device:";
			errMsg += SDL_GetError();
			cout << errMsg << endl;
			throw std::runtime_error(errMsg);
		}

		SDL_PauseAudioDevice(audioDeviceID, 0);
	}
}

void freeFileInfo() {
	if (fileInfo.pFrame) {
		av_frame_free(&fileInfo.pFrame);
	}
	if (fileInfo.pFrameYUV) {
		av_frame_free(&fileInfo.pFrameYUV);
	}
	if (fileInfo.pFormatCtx) {
		avformat_free_context(fileInfo.pFormatCtx);
	}

	if (fileInfo.audioCodecCtx) {
		avcodec_free_context(&fileInfo.audioCodecCtx);
	}

	if (fileInfo.videoCodecCtx) {
		avcodec_free_context(&fileInfo.videoCodecCtx);
	}
}

int main(int argc, char *argv[]) {
	
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		string errMsg = "Could not initialize SDL -";
		errMsg += SDL_GetError();
		cout << errMsg << endl;
		throw std::runtime_error(errMsg);
	}

	avdevice_register_all();

	packet_queue_init();
	initFileInfo("test.mp4");
	
	std::thread videoThread{ videoThreadFunc };
	std::thread grabThread{ grabFunc };
	std::thread audioThread{ audioThreadFunc };
	grabThread.detach();
	videoThread.detach();
	audioThread.detach();
	//SDL_Delay(300000);
	while (videoQ.nb_packets > 0 || audioQ.nb_packets > 0 || !fileInfo.fileEnd)
	{
		using namespace std::chrono_literals;
		std::this_thread::sleep_for(1000 * 1ms);
	}
	freeFileInfo();
	SDL_CloseAudio();
	SDL_Quit();
}