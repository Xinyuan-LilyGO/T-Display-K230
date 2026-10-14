#ifndef WEBRTC_AUDIO_IMPROVEMENT_H_
#define WEBRTC_AUDIO_IMPROVEMENT_H_

#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// 声音增加初始化
int webrtc_audio_quality_init(int sample_rate, int channel);

// 处理参考声音
int webrtc_process_reference_audio(char* ref_buf, int size);

// 增强音质
int webrtc_enhance_audio_quality(char* mic_buf, short *out_buf);

#ifdef __cplusplus
}
#endif

#endif  // WEBRTC_AUDIO_IMPROVEMENT_H_
