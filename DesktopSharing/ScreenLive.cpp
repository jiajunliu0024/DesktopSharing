﻿#include "ScreenLive.h"
#include "net/NetInterface.h"
#include "net/Timestamp.h"
#include "xop/RtspServer.h"
#include "xop/H264Parser.h"

ScreenLive::ScreenLive()
	: event_loop_(new xop::EventLoop)
{
	
}

ScreenLive::~ScreenLive()
{
	Destroy();
}

ScreenLive& ScreenLive::Instance()
{
	static ScreenLive s_screen_live;
	return s_screen_live;
}

bool ScreenLive::Init(AVConfig& config)
{
	if (is_initialized_) {
		Destroy();
	}
	
	av_config_ = config;

	if (StartCapture() < 0) {
		return false;
	}

	if (StartEncoder() < 0) {
		return false;
	}

	is_initialized_ = true;
	return true;
}

void ScreenLive::Destroy()
{
	if (is_initialized_)
	{
		std::lock_guard<std::mutex> locker(mutex_);

		if (rtsp_pusher_ != nullptr && rtsp_pusher_->IsConnected()) {
			rtsp_pusher_->Close();
			rtsp_pusher_ = nullptr;
		}

		if (rtmp_pusher_ != nullptr && rtmp_pusher_->isConnected()) {
			rtmp_pusher_->close();
			rtmp_pusher_ = nullptr;
		}

		if (rtsp_server_ != nullptr) {
			rtsp_server_->RemoveSession(media_session_id_);
			rtsp_server_ = nullptr;
		}
	}

	if (is_initialized_) {
		StopEncoder();
		StopCapture();
		is_initialized_ = false;
	}
}

bool ScreenLive::Start(int type, LiveConfig& config)
{
	if (!is_initialized_) {
		return false;
	}

	uint32_t samplerate = audio_capture_.GetSamplerate();
	uint32_t channels = audio_capture_.GetChannels();

	if (type == SCREEN_LIVE_RTSP_SERVER) {	
		auto rtsp_server = xop::RtspServer::Create(event_loop_.get());
		xop::MediaSessionId session_id = 0;
		if (!rtsp_server->Start(config.ip, config.port)) {
			return false;
		}

		xop::MediaSession* session = xop::MediaSession::CreateNew(config.suffix);
		session->AddSource(xop::channel_0, xop::H264Source::CreateNew());
		session->AddSource(xop::channel_1, xop::AACSource::CreateNew(samplerate, channels, false));
		session->SetNotifyCallback([this](xop::MediaSessionId sessionId, uint32_t clients) {
			printf("client: %u\n", clients);
			this->rtsp_clients_ = clients;
		});

		session_id = rtsp_server->AddSession(session);
		printf("RTSP Server: rtsp://%s:%hu/%s \n", xop::NetInterface::GetLocalIPAddress().c_str(), config.port, config.suffix.c_str());

		std::lock_guard<std::mutex> locker(mutex_);
		rtsp_server_ = rtsp_server;
		media_session_id_ = session_id;
	}
	else if (type == SCREEN_LIVE_RTSP_PUSHER) {
		auto rtsp_pusher = xop::RtspPusher::Create(event_loop_.get());
		xop::MediaSession *session = xop::MediaSession::CreateNew();
		session->AddSource(xop::channel_0, xop::H264Source::CreateNew());
		session->AddSource(xop::channel_1, xop::AACSource::CreateNew(audio_capture_.GetSamplerate(), audio_capture_.GetChannels(), false));
		
		rtsp_pusher->AddSession(session);
		if (rtsp_pusher->OpenUrl(config.rtsp_url, 3000) != 0) {
			rtsp_pusher = nullptr;
			printf("RTSP Pusher: Open %s failed. \n", config.rtsp_url.c_str());
			return false;
		}

		std::lock_guard<std::mutex> locker(mutex_);
		//if (rtsp_pusher_ != nullptr) {
		//	rtsp_pusher_->close();
		//}
		rtsp_pusher_ = rtsp_pusher;
		printf("RTSP Pusher: Push stream to  %s ... \n", config.rtsp_url.c_str());
	}
	else if (type == SCREEN_LIVE_RTMP_PUSHER) {

		auto rtmp_pusher = xop::RtmpPublisher::create(event_loop_.get());

		xop::MediaInfo mediaInfo;
		uint8_t* extradata = aac_encoder_.GetAVCodecContext()->extradata;
		uint8_t  extradata_size = aac_encoder_.GetAVCodecContext()->extradata_size;

		mediaInfo.audioSpecificConfigSize = extradata_size;
		mediaInfo.audioSpecificConfig.reset(new uint8_t[mediaInfo.audioSpecificConfigSize]);
		memcpy(mediaInfo.audioSpecificConfig.get(), extradata, extradata_size);

		uint8_t extradata_buf[1024];
		if (nvenc_data_ != nullptr) {
			extradata_size = nvenc_info.get_sequence_params(nvenc_data_, extradata_buf, 1024);
			extradata = extradata_buf;
		}
		else {
			extradata = h264_encoder.GetAVCodecContext()->extradata;
			extradata_size = h264_encoder.GetAVCodecContext()->extradata_size;
		}

		xop::Nal sps = xop::H264Parser::findNal(extradata, extradata_size);
		if (sps.first != nullptr && sps.second != nullptr && *sps.first == 0x67) {
			mediaInfo.spsSize = sps.second - sps.first + 1;
			mediaInfo.sps.reset(new uint8_t[mediaInfo.spsSize]);
			memcpy(mediaInfo.sps.get(), sps.first, mediaInfo.spsSize);

			xop::Nal pps = xop::H264Parser::findNal(sps.second, extradata_size - (sps.second - extradata));
			if (pps.first != nullptr && pps.second != nullptr && *pps.first == 0x68)
			{
				mediaInfo.ppsSize = pps.second - pps.first + 1;
				mediaInfo.pps.reset(new uint8_t[mediaInfo.ppsSize]);
				memcpy(mediaInfo.pps.get(), pps.first, mediaInfo.ppsSize);
			}
		}

		rtmp_pusher->setMediaInfo(mediaInfo);

		std::string status;
		if (rtmp_pusher->openUrl(config.rtmp_url, 2000, status) < 0) {
			printf("RTMP Pusher: Open %s failed. \n", config.rtmp_url.c_str());
			return false;
		}

		std::lock_guard<std::mutex> locker(mutex_);
		//if (rtmp_pusher_ != nullptr) {
		//	rtmp_pusher_->close();
		//}
		rtmp_pusher_ = rtmp_pusher;
		printf("RTMP Pusher: Push stream to  %s ... \n", config.rtmp_url.c_str());
	}
	else {
		return false;
	}

	return true;
}

void ScreenLive::Stop(int type)
{
	std::lock_guard<std::mutex> locker(mutex_);

	switch (type)
	{
	case SCREEN_LIVE_RTSP_SERVER:
		if (rtsp_server_ != nullptr) {
			rtsp_server_->Stop();
			rtsp_server_ = nullptr;
		}
		
		break;

	case SCREEN_LIVE_RTSP_PUSHER:
		if (rtsp_pusher_ != nullptr) {
			rtsp_pusher_->Close();
			rtsp_pusher_ = nullptr;
		}		
		break;

	case SCREEN_LIVE_RTMP_PUSHER:
		if (rtmp_pusher_ != nullptr) {
			rtmp_pusher_->close();
			rtmp_pusher_ = nullptr;
		}
		break;

	default:
		break;
	}
}

bool ScreenLive::IsConnected(int type)
{
	std::lock_guard<std::mutex> locker(mutex_);

	bool is_connected = false;
	switch (type)
	{
	case SCREEN_LIVE_RTSP_SERVER:
		if (rtsp_server_ != nullptr) {
			is_connected = rtsp_clients_ > 0;
		}		
		break;

	case SCREEN_LIVE_RTSP_PUSHER:
		if (rtsp_pusher_ != nullptr) {
			is_connected = rtsp_pusher_->IsConnected();
		}		
		break;

	case SCREEN_LIVE_RTMP_PUSHER:
		if (rtmp_pusher_ != nullptr) {
			is_connected = rtmp_pusher_->isConnected();
		}
		break;

	default:
		break;
	}

	return is_connected;
}

int ScreenLive::StartCapture()
{
	if (!screen_capture_.Init()) {
		return -1;
	}

	if (!audio_capture_.Init()) {
		return -1;
	}

	return 0;
}

int ScreenLive::StopCapture()
{
	screen_capture_.Destroy();
	audio_capture_.Destroy();
	return 0;
}

int ScreenLive::StartEncoder() 
{
	ffmpeg::AVConfig encoder_config;

	encoder_config.video.framerate = av_config_.framerate;
	encoder_config.video.bitrate = av_config_.bitrate_bps;
	encoder_config.video.gop = av_config_.gop;
	encoder_config.video.format = AV_PIX_FMT_BGRA;
	encoder_config.video.width = screen_capture_.GetWidth();
	encoder_config.video.height = screen_capture_.GetHeight();

	encoder_config.audio.samplerate = audio_capture_.GetSamplerate();
	encoder_config.audio.channels = audio_capture_.GetChannels();

	if (!h264_encoder.Init(encoder_config) ) {
		return -1;
	}

	if (!aac_encoder_.Init(encoder_config)) {
		return -1;
	}

	if (av_config_.codec == "h264_nvenc") {
		if (nvenc_info.is_supported()) {
			nvenc_data_ = nvenc_info.create();
		}

		if (nvenc_data_ != nullptr) {
			nvenc_config nvenc_config;
			nvenc_config.codec = "h264";
			nvenc_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
			nvenc_config.width = screen_capture_.GetWidth();
			nvenc_config.height = screen_capture_.GetHeight();
			nvenc_config.framerate = av_config_.framerate;
			nvenc_config.gop = av_config_.gop;
			nvenc_config.bitrate = av_config_.bitrate_bps;
			if (!nvenc_info.init(nvenc_data_, &nvenc_config)) {
				nvenc_info.destroy(&nvenc_data_);
				nvenc_data_ = nullptr;
			}
		}
	}

	is_started_ = true;
	encode_video_thread_.reset(new std::thread(&ScreenLive::EncodeVideo, this));
	encode_audio_thread_.reset(new std::thread(&ScreenLive::EncodeAudio, this));
	return 0;
}

int ScreenLive::StopEncoder() 
{
	is_started_ = false;

	if (encode_video_thread_) {		
		encode_video_thread_->join();
		encode_video_thread_ = nullptr;
	}

	if (encode_audio_thread_) {
		encode_audio_thread_->join();
		encode_audio_thread_ = nullptr;
	}

	h264_encoder.Destroy();
	aac_encoder_.Destroy();
	if (nvenc_data_ != nullptr) {
		nvenc_info.destroy(&nvenc_data_);
		nvenc_data_ = nullptr;
	}
	return 0;
}

bool ScreenLive::IsKeyFrame(const uint8_t* data, uint32_t size)
{
	if (size > 4) {
		//0x67:sps ,0x65:IDR, 0x6: SEI
		if (data[4] == 0x67 || data[4] == 0x65 || data[4] == 0x6) {
			return true;
		}
	}

	return false;
}

void ScreenLive::EncodeVideo()
{
	static xop::Timestamp tp, tp2;	
	uint32_t encode_fps = 0;
	uint32_t msec = 1000 / av_config_.framerate;

	uint32_t buffer_size = 1920 * 1080 * 4;				    
	std::shared_ptr<uint8_t> buffer(new uint8_t[buffer_size]);

	while (is_started_)
	{
		if (tp2.elapsed() >= 1000) {
			//printf("video fps: %d\n", fps); /*编码帧率统计*/
			tp2.reset();
			encode_fps = 0;
		}

		uint32_t delay = msec;
		uint32_t elapsed = (uint32_t)tp.elapsed(); /*编码耗时计算*/
		if (elapsed > delay) {
			delay = 0;
		}
		else {
			delay -= elapsed;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(delay));
		tp.reset();

		std::vector<uint8_t> bgra_image;
		uint32_t timestamp = xop::H264Source::GetTimestamp();
		int frame_size = 0;

		if (nvenc_data_ != nullptr) {
			HANDLE handle = nullptr;
			int lockKey = 0, unlockKey = 0;
			if (screen_capture_.GetTextureHandle(&handle, &lockKey, &unlockKey)) {
				frame_size = nvenc_info.encode_handle(nvenc_data_, handle, lockKey, unlockKey, buffer.get(), buffer_size);
				if (frame_size < 0) {
					if (screen_capture_.CaptureFrame(bgra_image)) {
						ID3D11Device* device = nvenc_info.get_device(nvenc_data_);
						ID3D11Texture2D* texture = nvenc_info.get_texture(nvenc_data_);
						ID3D11DeviceContext *context = nvenc_info.get_context(nvenc_data_);
						D3D11_MAPPED_SUBRESOURCE map;
						context->Map(texture, D3D11CalcSubresource(0, 0, 1), D3D11_MAP_WRITE, 0, &map);
						memcpy((uint8_t *)map.pData, &bgra_image[0], bgra_image.size());
						context->Unmap(texture, D3D11CalcSubresource(0, 0, 1));
						frame_size = nvenc_info.encode_texture(nvenc_data_, texture, buffer.get(), buffer_size);
					}
				}
			}		
		}
		else {
			if (screen_capture_.CaptureFrame(bgra_image)) {
				ffmpeg::AVPacketPtr pkt_ptr = h264_encoder.Encode(&bgra_image[0], screen_capture_.GetWidth(), screen_capture_.GetHeight());				
				int extra_data_size = 0;
				uint8_t* extra_data = nullptr;

				if (pkt_ptr != nullptr){
					if (IsKeyFrame(pkt_ptr->data, pkt_ptr->size)) {
						/* 编码器使用了AV_CODEC_FLAG_GLOBAL_HEADER, 这里需要添加sps, pps */
						extra_data = h264_encoder.GetAVCodecContext()->extradata;
						extra_data_size = h264_encoder.GetAVCodecContext()->extradata_size;
						memcpy(buffer.get(), extra_data, extra_data_size);
					}
					
					memcpy(buffer.get() + extra_data_size, pkt_ptr->data, pkt_ptr->size);
					frame_size = pkt_ptr->size + extra_data_size;
				}
			}				
		}		

		if (frame_size > 0) {
			PushVideo(buffer.get(), frame_size, timestamp);
		}
	}
}

void ScreenLive::EncodeAudio()
{
	std::shared_ptr<uint8_t> pcm_buffer(new uint8_t[48000 * 8]);	
	uint32_t frame_samples = aac_encoder_.GetFrameSamples();
	uint32_t channel = audio_capture_.GetChannels();
	uint32_t samplerate = audio_capture_.GetSamplerate();
	
	while (is_started_)
	{		
		if (audio_capture_.GetSamples() >= (int)frame_samples) {
			if (audio_capture_.Read(pcm_buffer.get(), frame_samples) != frame_samples) {
				continue;
			}

			ffmpeg::AVPacketPtr pkt_ptr = aac_encoder_.Encode(pcm_buffer.get(), frame_samples);
			if (pkt_ptr) {
				uint32_t timestamp = xop::AACSource::GetTimestamp(samplerate);
				PushAudio(pkt_ptr->data, pkt_ptr->size, timestamp);
			}
		}
		else {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}

void ScreenLive::PushVideo(const uint8_t* data, uint32_t size, uint32_t timestamp)
{
	xop::AVFrame vidoe_frame(size);
	vidoe_frame.size = size - 4; /* -4 去掉H.264起始码 */
	vidoe_frame.type = IsKeyFrame(data, size) ? xop::VIDEO_FRAME_I : xop::VIDEO_FRAME_P;
	vidoe_frame.timestamp = timestamp;
	memcpy(vidoe_frame.buffer.get(), data + 4, size - 4);

	if (size > 0)
	{
		std::lock_guard<std::mutex> locker(mutex_);

		/* RTSP服务器 */
		if (rtsp_server_ != nullptr && this->rtsp_clients_ > 0) {
			rtsp_server_->PushFrame(media_session_id_, xop::channel_0, vidoe_frame);
		}

		/* RTSP推流 */
		if (rtsp_pusher_ != nullptr && rtsp_pusher_->IsConnected()) {
			rtsp_pusher_->PushFrame(xop::channel_0, vidoe_frame);
		}

		/* RTMP推流 */
		if (rtmp_pusher_ != nullptr && rtmp_pusher_->isConnected()) {
			rtmp_pusher_->pushVideoFrame(vidoe_frame.buffer.get(), vidoe_frame.size);
		}
	}
}

void ScreenLive::PushAudio(const uint8_t* data, uint32_t size, uint32_t timestamp)
{
	xop::AVFrame audio_frame(size);
	audio_frame.timestamp = timestamp;
	audio_frame.type = xop::AUDIO_FRAME;
	audio_frame.size = size;
	memcpy(audio_frame.buffer.get(), data, size);

	if(size > 0)
	{
		std::lock_guard<std::mutex> locker(mutex_);

		/* RTSP服务器 */
		if (rtsp_server_ != nullptr && this->rtsp_clients_ > 0) {
			rtsp_server_->PushFrame(media_session_id_, xop::channel_1, audio_frame);
		}

		/* RTSP推流 */
		if (rtsp_pusher_ && rtsp_pusher_->IsConnected()) {
			rtsp_pusher_->PushFrame(xop::channel_1, audio_frame);
		}

		/* RTMP推流 */
		if (rtmp_pusher_ != nullptr && rtmp_pusher_->isConnected()) {
			rtmp_pusher_->pushAudioFrame(audio_frame.buffer.get(), audio_frame.size);
		}
	}
}