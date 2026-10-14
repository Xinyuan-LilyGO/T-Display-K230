#ifndef __AUDIO_ENHANCE_H__
#define __AUDIO_ENHANCE_H__
#include "webrtc-audio-processing-1/modules/audio_processing/include/audio_processing.h"

#ifdef __cplusplus
extern "C" {
#endif

// 定义 WEBRTC_AUDIO3A_AEC_CONFIG 结构体
typedef struct {
    std::unique_ptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig input_config;
    webrtc::StreamConfig output_config;
    int frameSize;    //每帧样本数
    int numChannels;  //声道数
    int sampleRate;   //采样率
} WEBRTC_AUDIO3A_AEC_CONFIG;

/**
 * @brief Initializes the AEC module with the given configuration.
 *
 * @param config Pointer to the AEC configuration structure.
 * @return int Status code (0 for success, non-zero for failure).
 */
int webrtc_audio3a_aec_init(WEBRTC_AUDIO3A_AEC_CONFIG* config);

/**
 * @brief Processes the audio signal for Acoustic Echo Cancellation (AEC).
 *
 * This function takes in microphone and speaker buffers, processes them
 * according to the provided AEC configuration, and outputs the processed
 * audio signal.
 *
 * @param config Pointer to the AEC configuration structure.
 * @param mic_buf Pointer to the buffer containing the microphone input signal.
 * @param spk_buf Pointer to the buffer containing the speaker input signal.
 * @param out_buf Pointer to the buffer where the processed output signal will be stored.
 *
 * @return An integer indicating the success or failure of the processing.
 */
int webrtc_audio3a_aec_process(WEBRTC_AUDIO3A_AEC_CONFIG* config,short *mic_buf, short *spk_buf, short *out_buf);

/**
 * @brief Destroys the AEC module and releases any allocated resources.
 *
 * @param config Pointer to the AEC configuration structure.
 * @return int Status code (0 for success, non-zero for failure).
 */
int webrtc_audio3a_aec_destroy(WEBRTC_AUDIO3A_AEC_CONFIG* config);

#ifdef __cplusplus
}
#endif

#endif // __AUDIO3A_AEC_H__