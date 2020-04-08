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

#pragma comment(lib, "setupapi.lib")
using namespace std;

#define VI_MAX_CAMERAS 20
DEFINE_GUID(CLSID_SystemDeviceEnum, 0x62be5d10, 0x60eb, 0x11d0, 0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86);
DEFINE_GUID(CLSID_VideoInputDeviceCategory, 0x860bb310, 0x5d01, 0x11d0, 0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86);
DEFINE_GUID(IID_ICreateDevEnum, 0x29840822, 0x5b84, 0x11d0, 0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86);

//列出硬件设备
int listDevices(vector<string>& list)
{
	ICreateDevEnum *pDevEnum = NULL;
	IEnumMoniker *pEnum = NULL;
	int deviceCounter = 0;
	CoInitialize(NULL);

	HRESULT hr = CoCreateInstance(
		CLSID_SystemDeviceEnum,
		NULL,
		CLSCTX_INPROC_SERVER,
		IID_ICreateDevEnum,
		reinterpret_cast<void**>(&pDevEnum)
	);

	if (SUCCEEDED(hr))
	{
		hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
		if (hr == S_OK) {

			IMoniker *pMoniker = NULL;
			while (pEnum->Next(1, &pMoniker, NULL) == S_OK)
			{
				IPropertyBag *pPropBag;
				hr = pMoniker->BindToStorage(0, 0, IID_IPropertyBag,
					(void**)(&pPropBag));

				if (FAILED(hr)) {
					pMoniker->Release();
					continue; // Skip this one, maybe the next one will work.
				}

				VARIANT varName;
				VariantInit(&varName);
				hr = pPropBag->Read(L"Description", &varName, 0);
				if (FAILED(hr))
				{
					hr = pPropBag->Read(L"FriendlyName", &varName, 0);
				}

				if (SUCCEEDED(hr))
				{
					hr = pPropBag->Read(L"FriendlyName", &varName, 0);
					int count = 0;
					char tmp[255] = { 0 };
					while (varName.bstrVal[count] != 0x00 && count < 255)
					{
						tmp[count] = (char)varName.bstrVal[count];
						count++;
					}
					list.push_back(tmp);
				}

				pPropBag->Release();
				pPropBag = NULL;
				pMoniker->Release();
				pMoniker = NULL;

				deviceCounter++;
			}

			pDevEnum->Release();
			pDevEnum = NULL;
			pEnum->Release();
			pEnum = NULL;
		}
	}
	return deviceCounter;
}

AVFormatContext *inputContext = nullptr;
AVCodecContext *pCodecCtx = nullptr;
AVCodec *codec = nullptr;
AVFrame	*pFrameYUV, *pFrame;
uint8_t *out_buffer;
AVPacket packet;
struct SwsContext *img_convert_ctx;

int screen_w, screen_h;
SDL_Window *screen;
SDL_Renderer* sdlRenderer;
SDL_Texture* sdlTexture;
SDL_Rect sdlRect;
SDL_Thread *video_tid;
SDL_Event event;

#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)

#define SFM_BREAK_EVENT  (SDL_USEREVENT + 2)

int thread_exit = 0;

int sfp_refresh_thread(void *opaque) {
	thread_exit = 0;
	while (!thread_exit) {
		SDL_Event event;
		event.type = SFM_REFRESH_EVENT;
		SDL_PushEvent(&event);
		SDL_Delay(10);
	}
	thread_exit = 0;
	//Break
	SDL_Event event;
	event.type = SFM_BREAK_EVENT;
	SDL_PushEvent(&event);

	return 0;
}

int openCamera() {
	inputContext = avformat_alloc_context();
	vector<string> videoDevices;
	listDevices(videoDevices);
	string camera = videoDevices[0];
	string readUrl = "helloteacher.avi";// +camera;
	//readUrl = "video=EasyCamera";
	AVInputFormat *ifmt = nullptr;//av_find_input_format("dshow");
	AVDictionary *format_opts = nullptr;
	//av_dict_set_int(&format_opts, "rtbufsize", 3041280 * 100, 0);
	//Set own video device's name
	if (avformat_open_input(&inputContext, readUrl.c_str(), ifmt, &format_opts) != 0) {
		printf("Couldn't open input stream.\n");
		return -1;
	}

	int ret = avformat_find_stream_info(inputContext, nullptr);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Find input file stream inform failed\n");
	}
	else
	{
		av_log(NULL, AV_LOG_FATAL, "Open input file  %s success\n", readUrl.c_str());
	}
	return ret;
}

int initCodecCtx(int videoIndex) {
	AVStream *st = inputContext->streams[videoIndex];
	codec = avcodec_find_decoder(st->codecpar->codec_id);

	if (!codec) {
		printf("could not open codec\n");
		return -1;
	}
	pCodecCtx = avcodec_alloc_context3(NULL);
	if (!pCodecCtx) {
		fprintf(stderr, "Could not allocate video codec context\n");
		return -1;
	}

	if ((avcodec_parameters_to_context(pCodecCtx, st->codecpar)) < 0) {
		fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
			av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
		return -1;
	}

	if (avcodec_open2(pCodecCtx, codec, NULL) < 0) {
		printf("open codec ctx fail\n");
		return -2;
	}
}

int sdlInit() {
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER)) {
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}
	//SDL 2.0 Support for multiple windows
	screen_w = pCodecCtx->width / 2;
	screen_h = pCodecCtx->height / 2;
	screen = SDL_CreateWindow("Simplest ffmpeg player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		screen_w, screen_h, SDL_WINDOW_OPENGL);

	if (!screen) {
		printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
		return -1;
	}
	sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
	//IYUV: Y + U + V  (3 planes)
	//YV12: Y + V + U  (3 planes)
	sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);

	sdlRect.x = 0;
	sdlRect.y = 0;
	sdlRect.w = screen_w;
	sdlRect.h = screen_h;
	return 0;
}

int main(int argc, char *argv[])
{
	//av_register_all();
	//打开电脑设备要注册
	int videoStream = -1;
	avdevice_register_all();
	avformat_network_init();
	if (openCamera() < 0) {
		goto __EXIT;
	}
	
	for (int i = 0; i < inputContext->nb_streams; ++i) {
		if (inputContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoStream = i;
		}
	}

	if (initCodecCtx(videoStream) < 0) {
		goto __EXIT;
	}

	pFrameYUV = av_frame_alloc();
	pFrame = av_frame_alloc();
	av_init_packet(&packet);
	out_buffer = (uint8_t *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 32)*sizeof(uint8_t));
	//pFrame->data, pFrame->linesize, buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height,1
	av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize,  out_buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 32);
	printf("%d\n", pCodecCtx->pix_fmt);
	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
		pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);


	if (sdlInit() < 0) {
		goto __EXIT;
	}
	video_tid = SDL_CreateThread(sfp_refresh_thread, NULL, NULL);
	while (true)
	{
		SDL_WaitEvent(&event);
		switch (event.type)
		{
		case SFM_REFRESH_EVENT: {

			int ret = -1;
			if ((ret = av_read_frame(inputContext, &packet)) >= 0) {
				cout << "pts: " << packet.pts << endl;
				if (packet.stream_index == videoStream) {
					int ret = avcodec_send_packet(pCodecCtx, &packet);

					if (ret != 0) {
						cout << "avcodec_send_packet fail" << endl;
						//prinitf("%s/n", "error"); 
					}
					int count = 0;
					while (avcodec_receive_frame(pCodecCtx, pFrame) == 0) {
						cout << "count: " << count << endl;
						count++;
						sws_scale(img_convert_ctx, (const uint8_t* const*)pFrame->data,
							pFrame->linesize, 0, pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);
						//读取到一帧音频或者视频
						//处理解码后音视频 frame
						//sws_scale(img_convert_ctx, (const uint8_t* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);
							//SDL---------------------------
						SDL_UpdateTexture(sdlTexture, NULL, pFrameYUV->data[0], pFrameYUV->linesize[0]);
						SDL_RenderClear(sdlRenderer);
						//SDL_RenderCopy( sdlRenderer, sdlTexture, &sdlRect, &sdlRect );  
						SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
						SDL_RenderPresent(sdlRenderer);
						//SDL End-----------------------
					}
				}
				
				av_packet_unref(&packet);
			}
			else {
				printf("read error:%d\n", ret);
				thread_exit = 1;
			}
		}break;
		case SDL_QUIT: {
			thread_exit = 1;
		}break;
		case SFM_BREAK_EVENT: {
			goto __EXIT;
		}break;

		default:
			break;
		}
	}


__EXIT:
	if (inputContext) {
		avformat_free_context(inputContext);
	}
	if (pCodecCtx) {
		avcodec_free_context(&pCodecCtx);
	}

	if (pFrameYUV) {
		av_frame_free(&pFrameYUV);
	}




	return 0;
}