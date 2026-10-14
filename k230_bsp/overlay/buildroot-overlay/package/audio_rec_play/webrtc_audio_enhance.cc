#include "webrtc_audio_enhance.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>

// 初始化函数
int webrtc_audio3a_aec_init(WEBRTC_AUDIO3A_AEC_CONFIG* config) {
    if (config == nullptr) {
        std::cerr << "config is nullptr" << std::endl;
        return -1;
    }
    // 创建 AudioProcessing 实例
    config->apm = std::unique_ptr<webrtc::AudioProcessing>(webrtc::AudioProcessingBuilder().Create());
    if (!config->apm) {
        std::cerr << "Failed to create AudioProcessing instance" << std::endl;
        return -1;
    }

    // 配置音频处理参数
    webrtc::AudioProcessing::Config apmConfig;
    apmConfig.echo_canceller.enabled = true;
    apmConfig.noise_suppression.enabled = true;
    apmConfig.gain_controller1.enabled = false;
    config->apm->ApplyConfig(apmConfig);

    // 使用结构体中的采样率参数，假设每帧 10 ms
    // config->frameSize = config->sampleRate / 100;  // 这行代码不再需要
    config->input_config = webrtc::StreamConfig(config->sampleRate, config->numChannels, false);
    config->output_config = webrtc::StreamConfig(config->sampleRate, config->numChannels, false);

    return 0;
}

// 处理函数
int webrtc_audio3a_aec_process(WEBRTC_AUDIO3A_AEC_CONFIG* config, short *mic_buf, short *spk_buf, short *out_buf) {
    if (config == nullptr || mic_buf == nullptr || out_buf == nullptr) {
        std::cerr << "Invalid pointers" << std::endl;
        return -1;
    }

    std::vector<int16_t> dest(config->frameSize);
    const int16_t* src = mic_buf;

    //处理扬声器音频数据（注入参考信号）
    if (spk_buf != nullptr) {
        const int16_t* speaker_src = spk_buf;
        config->apm->ProcessReverseStream(speaker_src, config->input_config, config->output_config, dest.data());
    }

    // 处理麦克风音频数据
    int ret = config->apm->ProcessStream(src, config->input_config, config->output_config, dest.data());
    if (ret != 0) {
        std::cerr << "Audio processing failed" << std::endl;
        return -1;
    }

    // 将处理后的音频数据复制到输出缓冲区
    for (int i = 0; i < config->frameSize; ++i) {
        out_buf[i] = dest[i];
    }

    return 0;
}

// 销毁函数
int webrtc_audio3a_aec_destroy(WEBRTC_AUDIO3A_AEC_CONFIG* config) {
    if (config == nullptr) {
        std::cerr << "config is nullptr" << std::endl;
        return -1;
    }
    config->apm.reset();
    return 0;
}
