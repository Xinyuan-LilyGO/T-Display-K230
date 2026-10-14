#include "webrtc_audio_improvement.h"
#include "webrtc_audio_enhance.h"
#include "audio_cfg.h"
#include <pthread.h>
#include <list>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_REF_BUF_NUM 3
typedef struct tag_pcm_buf_info
{
  short pcm_buffer[BUFFER_SAMPLES];
  int   pcm_index;
  bool  valid_buf;
}pcm_buf_info;
typedef std::list<pcm_buf_info> REF_BUF_LIST;

static pthread_mutex_t pcm_ref_mutex = PTHREAD_MUTEX_INITIALIZER;
static WEBRTC_AUDIO3A_AEC_CONFIG aec_config;
static REF_BUF_LIST pcm_ref_list;
static pcm_buf_info pcm_ref_buf;


int webrtc_audio_quality_init(int sample_rate, int channel)
{
    memset(&pcm_ref_buf,0,sizeof(pcm_ref_buf));
    aec_config.sampleRate = sample_rate;
    aec_config.numChannels = channel;
    // 每帧10ms
    aec_config.frameSize = aec_config.sampleRate / 100;

    if (0 != webrtc_audio3a_aec_init(&aec_config))
    {
        printf("audio3a_aec_init failed\n");
        return -1;
    }
    return 0;
}

static int ncount = 0;
int webrtc_process_reference_audio(char* ref_buf,int size)
{
    memcpy((char*)pcm_ref_buf.pcm_buffer+pcm_ref_buf.pcm_index, ref_buf, size);
    pcm_ref_buf.pcm_index += size;
    if (pcm_ref_buf.pcm_index >= BUFFER_SAMPLES*2) {
      pcm_ref_buf.pcm_index = 0;
      pcm_ref_buf.valid_buf = true;
      pthread_mutex_lock(&pcm_ref_mutex);
      pcm_ref_list.push_back(pcm_ref_buf);
      if (pcm_ref_list.size() > MAX_REF_BUF_NUM) {
        pcm_ref_list.pop_front();
        printf("[%d]play:PCM data list is full\n",ncount++);
      }
      pthread_mutex_unlock(&pcm_ref_mutex);

    }
    return 0;
}

int webrtc_enhance_audio_quality(char* mic_buf, short *out_buf)
{
    pcm_buf_info pcm_ref_tmp_buf;
    while(1)
    {
      pthread_mutex_lock(&pcm_ref_mutex);
      if (!pcm_ref_list.empty()) {
        pcm_ref_tmp_buf = pcm_ref_list.front();
        pcm_ref_list.pop_front();
        pthread_mutex_unlock(&pcm_ref_mutex);
        break;
      }
      pthread_mutex_unlock(&pcm_ref_mutex);
      usleep(1000*1);
    }

    int count = BUFFER_SAMPLES/aec_config.frameSize;
    for(int i = 0;i < count;i++)
    {
        webrtc_audio3a_aec_process(&aec_config, (short*)mic_buf + i*aec_config.frameSize, pcm_ref_tmp_buf.pcm_buffer + i*aec_config.frameSize, out_buf+i*aec_config.frameSize);
    }

    return 0;
}