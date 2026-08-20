#include "ui_audio.h"

#include "ui_hardware.h"
#include "ui_i18n.h"
#include "ui_input.h"
#include "ui_prefs.h"

#include <lvgl/src/misc/cache/instance/lv_image_cache.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_MAX_TRACKS 32
#define AUDIO_MAX_STREAMS 16
#define AUDIO_MAX_RECORDINGS 32
#define AUDIO_PATH_MAX 256
#define AUDIO_TITLE_MAX 96
#define AUDIO_META_MAX 48
#define AUDIO_STATUS_MAX 160
#define AUDIO_LOG_PATH "/tmp/k230_phone_audio.log"
#define AUDIO_DEBUG_LOG_PATH "/tmp/k230_phone_audio_debug.log"
#define AUDIO_RADIO_CONFIG_PATH "/root/radio_streams.txt"
#define AUDIO_VOLUME_CONFIG_PATH "/root/.k230_phone_audio_volume"
#define AUDIO_PCM_VOLUME_BIN "/root/app/k230_phone_ui/k230_pcm_volume"
#define AUDIO_RECORD_DIR "/root/recordings"
#define AUDIO_NOTIFICATION_DIR "/root/notification"
#define AUDIO_NOTIFICATION_PREF "audio.notification_sound"
#define AUDIO_NOTIFICATION_NONE "none"
#define AUDIO_NOTIFICATION_LOG_PATH "/tmp/k230_phone_notification.log"
#define AUDIO_RECORDER_LOG_PATH "/tmp/k230_phone_recorder.log"
#define AUDIO_COVER_PATH "/tmp/k230_phone_album.rgb565"
#define AUDIO_COVER_SIZE 456
#define AUDIO_COVER_BYTES (AUDIO_COVER_SIZE * AUDIO_COVER_SIZE * 2)
#define AUDIO_PCM_RATE 48000
#define AUDIO_APLAY_BUFFER_US 50000
#define AUDIO_APLAY_PERIOD_US 10000
#define AUDIO_VOLUME_MIN 0
#define AUDIO_VOLUME_MAX 45
#define AUDIO_VOLUME_DEFAULT 24
#define AUDIO_ICON_MIC "MIC"
#define AUDIO_ICON_REC "REC"
#define AUDIO_ICON_STOP "STOP"
#define AUDIO_MAX_NOTIFICATIONS 32

typedef struct {
    char path[AUDIO_PATH_MAX];
    char title[AUDIO_TITLE_MAX];
    char meta[AUDIO_META_MAX];
} audio_track_t;

typedef struct {
    char name[AUDIO_TITLE_MAX];
    char url[AUDIO_PATH_MAX];
} audio_stream_t;

typedef enum {
    MUSIC_MODE_REPEAT_ONE = 0,
    MUSIC_MODE_LIST_LOOP,
    MUSIC_MODE_SHUFFLE,
    MUSIC_MODE_COUNT,
} music_play_mode_t;

static const char *const music_dirs[] = {
    "/root/music",
    "/mnt/sdcard/music",
    "/mnt/music",
    "/media/music",
};

static const audio_stream_t default_streams[] = {
    {"Groove Salad", "http://ice3.somafm.com/groovesalad-128-mp3"},
    {"Drone Zone", "http://ice3.somafm.com/dronezone-128-mp3"},
    {"Deep Space One", "http://ice3.somafm.com/deepspaceone-128-mp3"},
    {"Secret Agent", "http://ice3.somafm.com/secretagent-128-mp3"},
};

static audio_track_t tracks[AUDIO_MAX_TRACKS];
static int track_count;
static int selected_track = -1;

static audio_stream_t streams[AUDIO_MAX_STREAMS];
static int stream_count;
static int selected_stream = -1;
static char custom_stream_url[AUDIO_PATH_MAX];

static audio_track_t recordings[AUDIO_MAX_RECORDINGS];
static int recording_count;
static int selected_recording = -1;
static audio_track_t notification_sounds[AUDIO_MAX_NOTIFICATIONS];
static int notification_sound_count;
static char notification_selected[AUDIO_TITLE_MAX];
static pid_t notification_pid = -1;
static char current_record_path[AUDIO_PATH_MAX];
static pid_t recorder_pid = -1;
static uint64_t recorder_start_us;
static int recorder_last_duration_sec;

static pid_t player_pid = -1;
static int player_paused;
static char player_title[AUDIO_TITLE_MAX] = "Idle";
static char player_status[AUDIO_STATUS_MAX] = "Ready";
static char player_source[AUDIO_META_MAX] = "--";
static char player_local_path[AUDIO_PATH_MAX];
static page_id_t active_audio_page = PAGE_HOME;
static music_play_mode_t music_play_mode = MUSIC_MODE_LIST_LOOP;
static int player_is_radio;
static int player_is_recording_file;
static int recorder_input_route_active;
static int recorder_playback_route_suspended;
static int audio_volume = AUDIO_VOLUME_DEFAULT;
static int audio_volume_loaded;
static uint64_t audio_last_volume_apply_us;
static int cover_track_index = -2;
static int cover_render_index = -2;
static uint8_t *cover_pixels;
static lv_image_dsc_t cover_image_dsc;
static int player_duration_sec;
static uint64_t player_start_us;
static uint64_t player_pause_start_us;
static uint64_t player_paused_total_us;
static int music_progress_dragging;

static lv_timer_t *audio_timer;
static lv_obj_t *music_title_label;
static lv_obj_t *music_meta_label;
static lv_obj_t *music_state_label;
static lv_obj_t *music_device_label;
static lv_obj_t *music_count_label;
static lv_obj_t *music_play_label;
static lv_obj_t *music_progress_slider;
static lv_obj_t *music_progress_label;
static lv_obj_t *music_cover_image;
static lv_obj_t *music_cover_placeholder;
static lv_obj_t *music_volume_slider;
static lv_obj_t *music_volume_label;
static lv_obj_t *music_list_overlay;
static lv_obj_t *music_info_overlay;
static lv_obj_t *music_info_title_label;
static lv_obj_t *music_info_path_label;
static lv_obj_t *music_info_duration_label;
static lv_obj_t *music_mode_btn[MUSIC_MODE_COUNT];
static lv_obj_t *music_row[AUDIO_MAX_TRACKS];
static lv_obj_t *music_row_icon[AUDIO_MAX_TRACKS];
static lv_obj_t *music_row_meta[AUDIO_MAX_TRACKS];
static lv_obj_t *radio_state_label;
static lv_obj_t *radio_network_label;
static lv_obj_t *radio_device_label;
static lv_obj_t *radio_count_label;
static lv_obj_t *radio_volume_slider;
static lv_obj_t *radio_volume_label;
static lv_obj_t *radio_row[AUDIO_MAX_STREAMS];
static lv_obj_t *radio_row_icon[AUDIO_MAX_STREAMS];
static lv_obj_t *recorder_timer_label;
static lv_obj_t *recorder_status_label;
static lv_obj_t *recorder_file_label;
static lv_obj_t *recorder_device_label;
static lv_obj_t *recorder_count_label;
static lv_obj_t *recorder_record_label;
static lv_obj_t *recorder_play_label;
static lv_obj_t *recorder_list_overlay;
static lv_obj_t *recorder_list_back;
static lv_obj_t *recorder_list_title;
static lv_obj_t *recorder_list_panel;
static lv_obj_t *recorder_delete_confirm_overlay;
static char recorder_delete_pending_path[AUDIO_PATH_MAX];
static char recorder_delete_pending_title[AUDIO_TITLE_MAX];
static lv_obj_t *recorder_row[AUDIO_MAX_RECORDINGS];
static lv_obj_t *recorder_row_icon[AUDIO_MAX_RECORDINGS];
static lv_obj_t *recorder_row_meta[AUDIO_MAX_RECORDINGS];
static lv_obj_t *notification_status_label;
static lv_obj_t *notification_current_label;
static lv_obj_t *notification_row[AUDIO_MAX_NOTIFICATIONS + 1];
static lv_obj_t *notification_row_meta[AUDIO_MAX_NOTIFICATIONS + 1];

static void audio_start_local(int index);
static void audio_start_local_at(int index, int start_sec);
static void audio_apply_volume_throttled(int force);
static void audio_notification_reap(void);
static void notification_refresh_ui(void);
static void music_refresh_ui(void);
static void recorder_refresh_ui(void);
static void recorder_rebuild_list_overlay(void);

static void audio_debug_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;

    fp = fopen(AUDIO_DEBUG_LOG_PATH, "a");
    if(!fp) {
        return;
    }

    fprintf(fp, "[%llu.%03llums] ",
            (unsigned long long)(ui_monotonic_us() / 1000ULL),
            (unsigned long long)(ui_monotonic_us() % 1000ULL));
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static long audio_file_size(const char *path)
{
    struct stat st;

    if(!path || stat(path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

static int audio_is_local_playback(void)
{
    return player_pid > 0 && !player_is_radio && selected_track >= 0;
}

static int audio_is_radio_playback(void)
{
    return player_pid > 0 && player_is_radio;
}

static int audio_is_recorder_playback(void)
{
    return player_pid > 0 && player_is_recording_file;
}

static void audio_set_status(const char *text)
{
    snprintf(player_status, sizeof(player_status), "%s", text ? text : "");
}

static int audio_shell_quote(const char *in, char *out, size_t out_len)
{
    size_t pos = 0;

    if(!in || !out || out_len < 3) {
        return -1;
    }

    out[pos++] = '\'';
    for(size_t i = 0; in[i] != '\0'; i++) {
        if(in[i] == '\'') {
            if(pos + 4 >= out_len) {
                break;
            }
            out[pos++] = '\'';
            out[pos++] = '\\';
            out[pos++] = '\'';
            out[pos++] = '\'';
        } else {
            if(pos + 2 >= out_len) {
                break;
            }
            out[pos++] = in[i];
        }
    }
    out[pos++] = '\'';
    out[pos] = '\0';
    return 0;
}

static int audio_has_command(const char *path)
{
    return path && access(path, X_OK) == 0;
}

static int audio_clamp_volume(int value)
{
    if(value < AUDIO_VOLUME_MIN) {
        return AUDIO_VOLUME_MIN;
    }
    if(value > AUDIO_VOLUME_MAX) {
        return AUDIO_VOLUME_MAX;
    }
    return value;
}

static int audio_read_mixer_volume(void)
{
    FILE *fp;
    char line[160];
    int seen_volume = 0;
    int value = -1;

    fp = popen("amixer contents 2>/dev/null", "r");
    if(fp) {
        while(fgets(line, sizeof(line), fp)) {
            if(strstr(line, "name='PCM Playback Volume'")) {
                seen_volume = 1;
                continue;
            }
            if(seen_volume && strstr(line, "values=")) {
                char *pos = strstr(line, "values=");
                value = atoi(pos + 7);
                break;
            }
        }
        pclose(fp);
    }

    return value < 0 ? -1 : audio_clamp_volume(value);
}

static int audio_read_saved_volume(void)
{
    char text[32];

    if(ui_read_file_first_line(AUDIO_VOLUME_CONFIG_PATH, text,
                               sizeof(text)) != 0) {
        return -1;
    }
    return audio_clamp_volume(atoi(text));
}

static void audio_save_volume(void)
{
    FILE *fp = fopen(AUDIO_VOLUME_CONFIG_PATH, "w");

    if(!fp) {
        return;
    }
    fprintf(fp, "%d\n", audio_volume);
    fclose(fp);
}

static void audio_load_volume_once(void)
{
    int value;

    if(audio_volume_loaded) {
        return;
    }

    value = audio_read_saved_volume();
    if(value < 0) {
        value = audio_read_mixer_volume();
    }
    if(value < 0) {
        value = AUDIO_VOLUME_DEFAULT;
    }

    audio_volume = audio_clamp_volume(value);
    audio_volume_loaded = 1;
    audio_apply_volume_throttled(1);
}

static void audio_update_volume_widgets(void)
{
    char text[24];
    int percent = (audio_volume * 100 + AUDIO_VOLUME_MAX / 2) / AUDIO_VOLUME_MAX;

    snprintf(text, sizeof(text), "%d%%", percent);
    if(music_volume_label) {
        lv_label_set_text(music_volume_label, text);
    }
    if(radio_volume_label) {
        lv_label_set_text(radio_volume_label, text);
    }
    if(music_volume_slider &&
       lv_slider_get_value(music_volume_slider) != audio_volume) {
        lv_slider_set_value(music_volume_slider, audio_volume, LV_ANIM_OFF);
    }
    if(radio_volume_slider &&
       lv_slider_get_value(radio_volume_slider) != audio_volume) {
        lv_slider_set_value(radio_volume_slider, audio_volume, LV_ANIM_OFF);
    }
}

static void audio_apply_volume(void)
{
    char cmd[160];
    int rc;

    audio_save_volume();
    snprintf(cmd, sizeof(cmd),
             "amixer -q cset numid=1 %d >/dev/null 2>&1; "
             "amixer -q cset numid=5 %d >/dev/null 2>&1",
             audio_volume, audio_volume);
    rc = system(cmd);
    (void)rc;
}

static void audio_apply_volume_throttled(int force)
{
    uint64_t now = ui_monotonic_us();

    if(force || audio_last_volume_apply_us == 0 ||
       now - audio_last_volume_apply_us >= 80000ULL) {
        audio_apply_volume();
        audio_last_volume_apply_us = now;
    }
}

static void audio_volume_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *slider = lv_event_get_target(event);

    if(code == LV_EVENT_VALUE_CHANGED) {
        audio_volume = audio_clamp_volume((int)lv_slider_get_value(slider));
        audio_update_volume_widgets();
        audio_apply_volume_throttled(0);
    } else if(code == LV_EVENT_RELEASED) {
        audio_volume = audio_clamp_volume((int)lv_slider_get_value(slider));
        audio_update_volume_widgets();
        audio_apply_volume_throttled(1);
    }
}

int ui_audio_get_volume_value(void)
{
    audio_load_volume_once();
    return audio_volume;
}

int ui_audio_get_volume_max(void)
{
    return AUDIO_VOLUME_MAX;
}

void ui_audio_set_volume_value(int value, int force)
{
    audio_load_volume_once();
    audio_volume = audio_clamp_volume(value);
    audio_update_volume_widgets();
    audio_apply_volume_throttled(force ? 1 : 0);
}

static int audio_read_card_status(char *buf, size_t len)
{
    FILE *fp;
    char line[160];
    int has_card = 0;

    if(!buf || len == 0) {
        return 0;
    }

    snprintf(buf, len, "No /proc/asound/cards");
    fp = fopen("/proc/asound/cards", "r");
    if(!fp) {
        return 0;
    }

    while(fgets(line, sizeof(line), fp)) {
        ui_trim_text(line);
        if(line[0] == '\0') {
            continue;
        }
        if(strstr(line, "no soundcards")) {
            snprintf(buf, len, "No soundcards");
            fclose(fp);
            return 0;
        }
        if(isdigit((unsigned char)line[0])) {
            snprintf(buf, len, "%s", line);
            has_card = 1;
            break;
        }
    }

    fclose(fp);
    if(!has_card) {
        snprintf(buf, len, "No soundcards");
    }
    return has_card;
}

static int audio_network_online(char *buf, size_t len)
{
    char ip[64];

    if(ui_read_iface_ip(NET_ETH_IFACE, ip, sizeof(ip)) == 0) {
        snprintf(buf, len, "Ethernet %s", ip);
        return 1;
    }
    if(ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) == 0) {
        snprintf(buf, len, "Wi-Fi %s", ip);
        return 1;
    }
    snprintf(buf, len, "Offline");
    return 0;
}

static const char *audio_basename(const char *path)
{
    const char *slash;

    if(!path) {
        return "";
    }

    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int audio_ends_with(const char *text, const char *suffix)
{
    size_t text_len;
    size_t suffix_len;

    if(!text || !suffix) {
        return 0;
    }

    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if(suffix_len > text_len) {
        return 0;
    }

    text += text_len - suffix_len;
    for(size_t i = 0; i < suffix_len; i++) {
        if(tolower((unsigned char)text[i]) !=
           tolower((unsigned char)suffix[i])) {
            return 0;
        }
    }
    return 1;
}

static int audio_supported_file(const char *name)
{
    return audio_ends_with(name, ".mp3") ||
           audio_ends_with(name, ".wav") ||
           audio_ends_with(name, ".flac") ||
           audio_ends_with(name, ".m4a") ||
           audio_ends_with(name, ".aac") ||
           audio_ends_with(name, ".ogg");
}

static const char *audio_format_label(const char *name)
{
    if(audio_ends_with(name, ".mp3")) {
        return "MP3";
    }
    if(audio_ends_with(name, ".wav")) {
        return "WAV";
    }
    if(audio_ends_with(name, ".flac")) {
        return "FLAC";
    }
    if(audio_ends_with(name, ".m4a")) {
        return "M4A";
    }
    if(audio_ends_with(name, ".aac")) {
        return "AAC";
    }
    if(audio_ends_with(name, ".ogg")) {
        return "OGG";
    }
    return "AUDIO";
}

static void audio_format_time(int seconds, char *buf, size_t len)
{
    if(seconds < 0) {
        seconds = 0;
    }
    snprintf(buf, len, "%d:%02d", seconds / 60, seconds % 60);
}

static int audio_probe_path_duration(const char *path)
{
    char quoted[AUDIO_PATH_MAX * 2];
    char cmd[AUDIO_PATH_MAX * 3];
    char text[32];
    FILE *fp;
    int seconds = 0;

    if(!path || !path[0]) {
        return 0;
    }
    if(audio_shell_quote(path, quoted, sizeof(quoted)) != 0) {
        return 0;
    }

    snprintf(cmd, sizeof(cmd),
             "ffmpeg -hide_banner -i %s 2>&1 | "
             "awk '/Duration:/ {gsub(/,/, \"\", $2); split($2,a,\":\"); "
             "print int(a[1]*3600+a[2]*60+a[3]); exit}'",
             quoted);
    fp = popen(cmd, "r");
    if(!fp) {
        return 0;
    }
    if(fgets(text, sizeof(text), fp)) {
        seconds = atoi(text);
    }
    pclose(fp);
    return seconds > 0 ? seconds : 0;
}

static int audio_probe_duration(int index)
{
    if(index < 0 || index >= track_count) {
        return 0;
    }
    return audio_probe_path_duration(tracks[index].path);
}

static int audio_current_elapsed_sec(void)
{
    uint64_t now;
    uint64_t paused_us;
    uint64_t elapsed_us;
    int seconds;

    if(player_pid <= 0 || player_start_us == 0 || player_is_radio) {
        return 0;
    }

    now = player_paused && player_pause_start_us ? player_pause_start_us :
          ui_monotonic_us();
    paused_us = player_paused_total_us;
    if(player_paused && player_pause_start_us &&
       now > player_pause_start_us) {
        paused_us += now - player_pause_start_us;
    }
    if(now <= player_start_us + paused_us) {
        return 0;
    }

    elapsed_us = now - player_start_us - paused_us;
    seconds = (int)(elapsed_us / 1000000ULL);
    if(player_duration_sec > 0 && seconds > player_duration_sec) {
        seconds = player_duration_sec;
    }
    return seconds;
}

static void music_update_progress(void)
{
    char elapsed_text[16];
    char duration_text[16];
    char label[40];
    int elapsed = audio_current_elapsed_sec();
    int range = player_duration_sec > 0 ? player_duration_sec : 100;

    if(music_progress_dragging && music_progress_slider) {
        elapsed = (int)lv_slider_get_value(music_progress_slider);
    }
    if(music_progress_slider) {
        lv_slider_set_range(music_progress_slider, 0, range);
        if(!music_progress_dragging) {
            lv_slider_set_value(music_progress_slider,
                                player_duration_sec > 0 ? elapsed : 0,
                                LV_ANIM_OFF);
        }
    }
    if(music_progress_label) {
        if(player_duration_sec > 0) {
            audio_format_time(elapsed, elapsed_text, sizeof(elapsed_text));
            audio_format_time(player_duration_sec, duration_text,
                              sizeof(duration_text));
            snprintf(label, sizeof(label), "%s / %s", elapsed_text,
                     duration_text);
        } else {
            snprintf(label, sizeof(label), "--:-- / --:--");
        }
        lv_label_set_text(music_progress_label, label);
    }
}

static void music_progress_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if(!music_progress_slider || player_duration_sec <= 0 || track_count <= 0 ||
       selected_track < 0) {
        return;
    }

    if(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        music_progress_dragging = 1;
        music_update_progress();
    } else if(code == LV_EVENT_VALUE_CHANGED) {
        if(music_progress_dragging) {
            music_update_progress();
        }
    } else if(code == LV_EVENT_RELEASED) {
        int seek_sec = (int)lv_slider_get_value(music_progress_slider);

        music_progress_dragging = 0;
        if(seek_sec < 0) {
            seek_sec = 0;
        }
        if(seek_sec >= player_duration_sec) {
            seek_sec = player_duration_sec > 1 ? player_duration_sec - 1 : 0;
        }
        audio_debug_log("SEEK_REQUEST index=%d seek_sec=%d duration=%d",
                        selected_track, seek_sec, player_duration_sec);
        audio_start_local_at(selected_track, seek_sec);
        music_refresh_ui();
    }
}

static void audio_extract_cover(int index)
{
    char quoted[AUDIO_PATH_MAX * 2];
    char cmd[AUDIO_PATH_MAX * 3];
    int rc;

    unlink(AUDIO_COVER_PATH);
    unlink("/tmp/k230_phone_album.jpg");
    unlink("/tmp/k230_phone_album.png");
    if(index < 0 || index >= track_count) {
        audio_debug_log("COVER_SKIP invalid_index=%d track_count=%d", index,
                        track_count);
        return;
    }
    audio_debug_log("COVER_EXTRACT_BEGIN index=%d title=\"%s\" path=\"%s\"",
                    index, tracks[index].title, tracks[index].path);
    if(!audio_has_command("/usr/bin/ffmpeg")) {
        audio_debug_log("COVER_EXTRACT_FAIL ffmpeg_missing");
        return;
    }
    if(audio_shell_quote(tracks[index].path, quoted, sizeof(quoted)) != 0) {
        audio_debug_log("COVER_EXTRACT_FAIL quote_error");
        return;
    }

    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -nostdin -hide_banner -loglevel error -i %s "
             "-map 0:v:0 -an -frames:v 1 "
             "-vf 'scale=%d:%d:force_original_aspect_ratio=decrease,"
             "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=0x182331' "
             "-pix_fmt rgb565le -f rawvideo "
             AUDIO_COVER_PATH " >>" AUDIO_DEBUG_LOG_PATH " 2>&1",
             quoted, AUDIO_COVER_SIZE, AUDIO_COVER_SIZE, AUDIO_COVER_SIZE,
             AUDIO_COVER_SIZE);
    rc = system(cmd);
    audio_debug_log("COVER_EXTRACT_END rc=%d exit=%d size=%ld exists=%d",
                    rc, ui_shell_exit_code(rc),
                    audio_file_size(AUDIO_COVER_PATH),
                    ui_path_exists(AUDIO_COVER_PATH));
}

static int audio_load_cover_pixels(void)
{
    FILE *fp;
    size_t bytes_read;

    if(audio_file_size(AUDIO_COVER_PATH) != AUDIO_COVER_BYTES) {
        audio_debug_log("COVER_RAW_BAD_SIZE path=%s size=%ld expected=%d",
                        AUDIO_COVER_PATH, audio_file_size(AUDIO_COVER_PATH),
                        AUDIO_COVER_BYTES);
        return 0;
    }

    if(!cover_pixels) {
        cover_pixels = malloc(AUDIO_COVER_BYTES);
        if(!cover_pixels) {
            audio_debug_log("COVER_RAW_ALLOC_FAIL bytes=%d", AUDIO_COVER_BYTES);
            return 0;
        }
    }

    fp = fopen(AUDIO_COVER_PATH, "rb");
    if(!fp) {
        audio_debug_log("COVER_RAW_OPEN_FAIL path=%s", AUDIO_COVER_PATH);
        return 0;
    }
    bytes_read = fread(cover_pixels, 1, AUDIO_COVER_BYTES, fp);
    fclose(fp);

    if(bytes_read != AUDIO_COVER_BYTES) {
        audio_debug_log("COVER_RAW_READ_FAIL bytes=%zu expected=%d",
                        bytes_read, AUDIO_COVER_BYTES);
        return 0;
    }

    memset(&cover_image_dsc, 0, sizeof(cover_image_dsc));
    cover_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    cover_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    cover_image_dsc.header.w = AUDIO_COVER_SIZE;
    cover_image_dsc.header.h = AUDIO_COVER_SIZE;
    cover_image_dsc.header.stride = AUDIO_COVER_SIZE * 2;
    cover_image_dsc.data_size = AUDIO_COVER_BYTES;
    cover_image_dsc.data = cover_pixels;

    audio_debug_log("COVER_RAW_READY path=%s bytes=%d cf=%u stride=%u",
                    AUDIO_COVER_PATH, AUDIO_COVER_BYTES,
                    (unsigned)cover_image_dsc.header.cf,
                    (unsigned)cover_image_dsc.header.stride);
    return 1;
}

static void music_update_cover(void)
{
    int index = selected_track;
    int has_cover;

    if(index < 0 && track_count > 0) {
        index = 0;
    }
    if(index != cover_track_index) {
        audio_extract_cover(index);
        cover_track_index = index;
    }

    if(!music_cover_image || !music_cover_placeholder) {
        return;
    }

    has_cover = ui_path_exists(AUDIO_COVER_PATH);
    if(cover_render_index == index) {
        return;
    }
    cover_render_index = index;

    if(has_cover && audio_load_cover_pixels()) {
        lv_image_cache_drop(&cover_image_dsc);
        lv_image_set_src(music_cover_image, &cover_image_dsc);
        lv_obj_clear_flag(music_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(music_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
        audio_debug_log("COVER_RAW_SET hidden=%d size=%ld",
                        lv_obj_has_flag(music_cover_image, LV_OBJ_FLAG_HIDDEN),
                        audio_file_size(AUDIO_COVER_PATH));
    } else {
        lv_image_set_src(music_cover_image, NULL);
        lv_obj_add_flag(music_cover_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(music_cover_placeholder, LV_OBJ_FLAG_HIDDEN);
        audio_debug_log("COVER_PLACEHOLDER no_cover index=%d", index);
    }
}

static void audio_seed_shuffle(void)
{
    static int seeded;

    if(!seeded) {
        srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
        seeded = 1;
    }
}

static int music_next_index_for_mode(void)
{
    int next;

    if(track_count <= 0) {
        return -1;
    }
    if(selected_track < 0) {
        return 0;
    }
    if(music_play_mode == MUSIC_MODE_REPEAT_ONE) {
        return selected_track;
    }
    if(music_play_mode == MUSIC_MODE_SHUFFLE && track_count > 1) {
        audio_seed_shuffle();
        next = rand() % track_count;
        if(next == selected_track) {
            next = (next + 1) % track_count;
        }
        return next;
    }

    next = selected_track + 1;
    return next >= track_count ? 0 : next;
}

static int music_prev_index_for_mode(void)
{
    if(track_count <= 0) {
        return -1;
    }
    if(selected_track < 0) {
        return 0;
    }
    if(music_play_mode == MUSIC_MODE_SHUFFLE && track_count > 1) {
        audio_seed_shuffle();
        return rand() % track_count;
    }
    return selected_track > 0 ? selected_track - 1 : track_count - 1;
}

static int audio_track_compare(const void *a, const void *b)
{
    const audio_track_t *left = (const audio_track_t *)a;
    const audio_track_t *right = (const audio_track_t *)b;

    return strcasecmp(left->title, right->title);
}

static void audio_add_track(const char *path)
{
    audio_track_t *track;

    if(track_count >= AUDIO_MAX_TRACKS || !path) {
        return;
    }

    track = &tracks[track_count++];
    snprintf(track->path, sizeof(track->path), "%s", path);
    snprintf(track->title, sizeof(track->title), "%s", audio_basename(path));
    snprintf(track->meta, sizeof(track->meta), "%s", audio_format_label(path));
}

static void audio_scan_dir(const char *dir, int depth)
{
    DIR *dp;
    struct dirent *entry;

    if(track_count >= AUDIO_MAX_TRACKS || depth < 0 || !dir) {
        return;
    }

    dp = opendir(dir);
    if(!dp) {
        return;
    }

    while((entry = readdir(dp)) != NULL && track_count < AUDIO_MAX_TRACKS) {
        char path[AUDIO_PATH_MAX];
        struct stat st;

        if(strcmp(entry->d_name, ".") == 0 ||
           strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if(stat(path, &st) != 0) {
            continue;
        }

        if(S_ISDIR(st.st_mode) && depth > 0) {
            audio_scan_dir(path, depth - 1);
        } else if(S_ISREG(st.st_mode) && audio_supported_file(entry->d_name)) {
            audio_add_track(path);
        }
    }

    closedir(dp);
}

static void audio_scan_tracks(void)
{
    char keep_path[AUDIO_PATH_MAX] = "";

    if(player_local_path[0]) {
        snprintf(keep_path, sizeof(keep_path), "%s", player_local_path);
    } else if(selected_track >= 0 && selected_track < track_count) {
        snprintf(keep_path, sizeof(keep_path), "%s", tracks[selected_track].path);
    }

    track_count = 0;
    selected_track = -1;
    cover_track_index = -2;
    cover_render_index = -2;
    memset(tracks, 0, sizeof(tracks));

    for(size_t i = 0; i < sizeof(music_dirs) / sizeof(music_dirs[0]); i++) {
        audio_scan_dir(music_dirs[i], 2);
    }
    if(track_count > 1) {
        qsort(tracks, (size_t)track_count, sizeof(tracks[0]),
              audio_track_compare);
    }
    if(keep_path[0]) {
        for(int i = 0; i < track_count; i++) {
            if(strcmp(tracks[i].path, keep_path) == 0) {
                selected_track = i;
                break;
            }
        }
    }
    if(selected_track < 0 && track_count > 0) {
        selected_track = 0;
    }
}

static int notification_name_valid(const char *name)
{
    if(!name || !name[0] || strchr(name, '/') || strstr(name, "..")) {
        return 0;
    }
    return audio_supported_file(name);
}

static void notification_make_path(const char *name, char *path, size_t len)
{
    snprintf(path, len, "%s/%s", AUDIO_NOTIFICATION_DIR, name ? name : "");
}

static void notification_scan_sounds(void)
{
    DIR *dp;
    struct dirent *entry;

    notification_sound_count = 0;
    memset(notification_sounds, 0, sizeof(notification_sounds));

    dp = opendir(AUDIO_NOTIFICATION_DIR);
    if(!dp) {
        return;
    }

    while((entry = readdir(dp)) != NULL &&
          notification_sound_count < AUDIO_MAX_NOTIFICATIONS) {
        char path[AUDIO_PATH_MAX];
        struct stat st;
        audio_track_t *item;

        if(strcmp(entry->d_name, ".") == 0 ||
           strcmp(entry->d_name, "..") == 0 ||
           !notification_name_valid(entry->d_name)) {
            continue;
        }

        notification_make_path(entry->d_name, path, sizeof(path));
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        item = &notification_sounds[notification_sound_count++];
        snprintf(item->path, sizeof(item->path), "%s", path);
        snprintf(item->title, sizeof(item->title), "%s", entry->d_name);
        snprintf(item->meta, sizeof(item->meta), "%s  %ld KB",
                 audio_format_label(entry->d_name),
                 (long)((st.st_size + 1023) / 1024));
    }

    closedir(dp);
    if(notification_sound_count > 1) {
        qsort(notification_sounds, (size_t)notification_sound_count,
              sizeof(notification_sounds[0]), audio_track_compare);
    }
}

static int notification_find_index(const char *name)
{
    if(!name || !name[0]) {
        return -1;
    }
    for(int i = 0; i < notification_sound_count; i++) {
        if(strcmp(notification_sounds[i].title, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int notification_default_index(void)
{
    int idx = notification_find_index("notification01.mp3");

    if(idx >= 0) {
        return idx;
    }
    return notification_sound_count > 0 ? 0 : -1;
}

static int notification_load_selected(char *name, size_t len)
{
    char value[AUDIO_TITLE_MAX];
    int idx;

    if(!name || len == 0) {
        return -1;
    }

    name[0] = '\0';
    ui_prefs_get(AUDIO_NOTIFICATION_PREF, value, sizeof(value), "");
    ui_trim_text(value);
    if(strcmp(value, AUDIO_NOTIFICATION_NONE) == 0) {
        snprintf(name, len, "%s", AUDIO_NOTIFICATION_NONE);
        return -1;
    }

    notification_scan_sounds();
    if(notification_name_valid(value)) {
        idx = notification_find_index(value);
        if(idx >= 0) {
            snprintf(name, len, "%s", notification_sounds[idx].title);
            return idx;
        }
    }

    idx = notification_default_index();
    if(idx >= 0) {
        snprintf(name, len, "%s", notification_sounds[idx].title);
    }
    return idx;
}

static int recorder_file_supported(const char *name)
{
    return audio_ends_with(name, ".wav");
}

static int recorder_track_compare(const void *a, const void *b)
{
    const audio_track_t *left = (const audio_track_t *)a;
    const audio_track_t *right = (const audio_track_t *)b;

    return strcasecmp(right->path, left->path);
}

static void recorder_add_file(const char *path)
{
    audio_track_t *item;
    int duration;

    if(recording_count >= AUDIO_MAX_RECORDINGS || !path) {
        return;
    }

    item = &recordings[recording_count++];
    snprintf(item->path, sizeof(item->path), "%s", path);
    snprintf(item->title, sizeof(item->title), "%s", audio_basename(path));
    duration = audio_probe_path_duration(path);
    if(duration > 0) {
        char text[24];

        audio_format_time(duration, text, sizeof(text));
        snprintf(item->meta, sizeof(item->meta), "WAV  %s", text);
    } else {
        snprintf(item->meta, sizeof(item->meta), "%s", "WAV");
    }
}

static void recorder_scan_recordings(void)
{
    DIR *dp;
    struct dirent *entry;
    char keep_path[AUDIO_PATH_MAX] = "";

    if(selected_recording >= 0 && selected_recording < recording_count) {
        snprintf(keep_path, sizeof(keep_path), "%s",
                 recordings[selected_recording].path);
    } else if(current_record_path[0]) {
        snprintf(keep_path, sizeof(keep_path), "%s", current_record_path);
    }

    recording_count = 0;
    selected_recording = -1;
    memset(recordings, 0, sizeof(recordings));

    dp = opendir(AUDIO_RECORD_DIR);
    if(!dp) {
        return;
    }

    while((entry = readdir(dp)) != NULL &&
          recording_count < AUDIO_MAX_RECORDINGS) {
        char path[AUDIO_PATH_MAX];
        struct stat st;

        if(strcmp(entry->d_name, ".") == 0 ||
           strcmp(entry->d_name, "..") == 0 ||
           !recorder_file_supported(entry->d_name)) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", AUDIO_RECORD_DIR, entry->d_name);
        if(stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 44) {
            recorder_add_file(path);
        }
    }
    closedir(dp);

    if(recording_count > 1) {
        qsort(recordings, (size_t)recording_count, sizeof(recordings[0]),
              recorder_track_compare);
    }
    if(keep_path[0]) {
        for(int i = 0; i < recording_count; i++) {
            if(strcmp(recordings[i].path, keep_path) == 0) {
                selected_recording = i;
                break;
            }
        }
    }
    if(selected_recording < 0 && recording_count > 0) {
        selected_recording = 0;
    }
}

static void audio_load_streams(void)
{
    FILE *fp;
    char line[384];

    stream_count = 0;
    selected_stream = -1;
    memset(streams, 0, sizeof(streams));

    for(size_t i = 0; i < sizeof(default_streams) / sizeof(default_streams[0]) &&
                    stream_count < AUDIO_MAX_STREAMS; i++) {
        streams[stream_count++] = default_streams[i];
    }

    fp = fopen(AUDIO_RADIO_CONFIG_PATH, "r");
    if(!fp) {
        return;
    }

    while(fgets(line, sizeof(line), fp) && stream_count < AUDIO_MAX_STREAMS) {
        char *sep;
        char *name = line;
        char *url;

        ui_trim_text(line);
        if(line[0] == '\0' || line[0] == '#') {
            continue;
        }

        sep = strchr(line, '|');
        if(!sep) {
            continue;
        }
        *sep = '\0';
        url = sep + 1;
        ui_trim_text(name);
        ui_trim_text(url);
        if(name[0] == '\0' || strncmp(url, "http://", 7) != 0) {
            continue;
        }

        snprintf(streams[stream_count].name, sizeof(streams[stream_count].name),
                 "%s", name);
        snprintf(streams[stream_count].url, sizeof(streams[stream_count].url),
                 "%s", url);
        stream_count++;
    }

    fclose(fp);
}

static void audio_terminate_process(void)
{
    int status;

    if(player_pid <= 0) {
        return;
    }

    kill(-player_pid, SIGTERM);
    for(int i = 0; i < 10; i++) {
        if(waitpid(player_pid, &status, WNOHANG) == player_pid) {
            player_pid = -1;
            player_paused = 0;
            player_is_radio = 0;
            player_is_recording_file = 0;
            player_start_us = 0;
            player_pause_start_us = 0;
            player_paused_total_us = 0;
            return;
        }
        usleep(20000);
    }

    kill(-player_pid, SIGKILL);
    waitpid(player_pid, &status, 0);
    player_pid = -1;
    player_paused = 0;
    player_is_radio = 0;
    player_is_recording_file = 0;
    player_start_us = 0;
    player_pause_start_us = 0;
    player_paused_total_us = 0;
}

static void audio_poll_process(void)
{
    int status;
    pid_t rc;

    if(player_pid <= 0) {
        return;
    }

    rc = waitpid(player_pid, &status, WNOHANG);
    if(rc == 0) {
        audio_set_status(player_paused ? "Paused" : "Playing");
        return;
    }

    if(rc == player_pid) {
        int was_local = !player_is_radio && !player_is_recording_file &&
                        selected_track >= 0;
        int ended_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

        player_pid = -1;
        player_paused = 0;
        player_is_radio = 0;
        player_is_recording_file = 0;
        player_start_us = 0;
        player_pause_start_us = 0;
        player_paused_total_us = 0;

        if(was_local && ended_ok && track_count > 0) {
            int next = music_next_index_for_mode();
            if(next >= 0) {
                audio_start_local(next);
                return;
            }
        }

        if(ended_ok) {
            audio_set_status("Stopped");
        } else {
            audio_set_status("Playback failed");
        }
    }
}

static int audio_prepare_playback(void)
{
    char device[128];

    if(!audio_read_card_status(device, sizeof(device))) {
        audio_set_status(device);
        return 0;
    }
    if(!audio_has_command("/usr/bin/ffmpeg")) {
        audio_set_status("ffmpeg missing");
        return 0;
    }
    if(!audio_has_command("/usr/bin/aplay")) {
        audio_set_status("aplay missing");
        return 0;
    }
    if(!audio_has_command(AUDIO_PCM_VOLUME_BIN)) {
        audio_set_status("pcm volume helper missing");
        return 0;
    }
    return 1;
}

static int audio_start_command(const char *cmd, const char *title,
                               const char *source)
{
    pid_t pid;

    if(!cmd || !title || !source) {
        return -1;
    }
    if(!audio_prepare_playback()) {
        return -1;
    }

    audio_apply_volume_throttled(1);
    audio_terminate_process();

    pid = fork();
    if(pid < 0) {
        audio_set_status("fork failed");
        return -1;
    }

    if(pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    setpgid(pid, pid);
    player_pid = pid;
    player_paused = 0;
    player_start_us = ui_monotonic_us();
    player_pause_start_us = 0;
    player_paused_total_us = 0;
    snprintf(player_title, sizeof(player_title), "%s", title);
    snprintf(player_source, sizeof(player_source), "%s", source);
    audio_set_status("Starting");
    return 0;
}

static void audio_notification_reap(void)
{
    int status;

    if(notification_pid <= 0) {
        return;
    }
    if(waitpid(notification_pid, &status, WNOHANG) == notification_pid) {
        notification_pid = -1;
    }
}

static void audio_notification_stop(void)
{
    int status;

    audio_notification_reap();
    if(notification_pid <= 0) {
        return;
    }
    kill(-notification_pid, SIGTERM);
    usleep(80000);
    if(waitpid(notification_pid, &status, WNOHANG) == notification_pid) {
        notification_pid = -1;
        return;
    }
    kill(-notification_pid, SIGKILL);
    waitpid(notification_pid, &status, 0);
    notification_pid = -1;
}

static int audio_play_notification_path(const char *path)
{
    char quoted[AUDIO_PATH_MAX * 2];
    char cmd[AUDIO_PATH_MAX * 4];
    pid_t pid;

    audio_notification_reap();
    if(notification_pid > 0) {
        audio_notification_stop();
    }
    if(!path || access(path, R_OK) != 0) {
        audio_debug_log("NOTIFICATION_SKIP reason=missing path=\"%s\"",
                        path ? path : "");
        return -1;
    }
    if(!audio_has_command("/usr/bin/ffmpeg") ||
       !audio_has_command("/usr/bin/aplay") ||
       !audio_has_command(AUDIO_PCM_VOLUME_BIN)) {
        audio_debug_log("NOTIFICATION_SKIP reason=missing-command");
        return -1;
    }
    if(audio_shell_quote(path, quoted, sizeof(quoted)) != 0) {
        audio_debug_log("NOTIFICATION_SKIP reason=quote path=\"%s\"", path);
        return -1;
    }

    audio_load_volume_once();
    audio_apply_volume_throttled(1);
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -nostdin -hide_banner -loglevel error -i %s "
             "-vn -f s16le -acodec pcm_s16le -ac 2 -ar %d - "
             "2>" AUDIO_NOTIFICATION_LOG_PATH " | "
             AUDIO_PCM_VOLUME_BIN " " AUDIO_VOLUME_CONFIG_PATH
             " 2>>" AUDIO_NOTIFICATION_LOG_PATH " | "
             "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
             "-B %d -F %d - >>" AUDIO_NOTIFICATION_LOG_PATH " 2>&1",
             quoted, AUDIO_PCM_RATE, AUDIO_PCM_RATE,
             AUDIO_APLAY_BUFFER_US, AUDIO_APLAY_PERIOD_US);

    pid = fork();
    if(pid < 0) {
        audio_debug_log("NOTIFICATION_SKIP reason=fork");
        return -1;
    }
    if(pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    notification_pid = pid;
    audio_debug_log("NOTIFICATION_PLAY pid=%d path=\"%s\"", (int)pid, path);
    return 0;
}

void ui_audio_play_notification(void)
{
    char name[AUDIO_TITLE_MAX];
    char path[AUDIO_PATH_MAX];
    int idx;

    idx = notification_load_selected(name, sizeof(name));
    if(idx < 0 || strcmp(name, AUDIO_NOTIFICATION_NONE) == 0) {
        audio_debug_log("NOTIFICATION_SKIP reason=disabled");
        return;
    }
    notification_make_path(name, path, sizeof(path));
    (void)audio_play_notification_path(path);
}

static void audio_toggle_pause(void)
{
    if(player_pid <= 0) {
        return;
    }

    if(player_paused) {
        uint64_t now = ui_monotonic_us();

        if(player_pause_start_us && now > player_pause_start_us) {
            player_paused_total_us += now - player_pause_start_us;
        }
        player_pause_start_us = 0;
        kill(-player_pid, SIGCONT);
        player_paused = 0;
        audio_set_status("Playing");
    } else {
        kill(-player_pid, SIGSTOP);
        player_paused = 1;
        player_pause_start_us = ui_monotonic_us();
        audio_set_status("Paused");
    }
}

static void audio_start_local_at(int index, int start_sec)
{
    char quoted[AUDIO_PATH_MAX * 2];
    char cmd[AUDIO_PATH_MAX * 4];

    if(index < 0 || index >= track_count) {
        return;
    }

    selected_track = index;
    selected_stream = -1;
    if(start_sec < 0) {
        start_sec = 0;
    }
    snprintf(player_local_path, sizeof(player_local_path), "%s",
             tracks[index].path);
    player_duration_sec = audio_probe_duration(index);
    if(player_duration_sec > 0 && start_sec >= player_duration_sec) {
        start_sec = player_duration_sec > 1 ? player_duration_sec - 1 : 0;
    }
    if(audio_shell_quote(tracks[index].path, quoted, sizeof(quoted)) != 0) {
        audio_set_status("Path error");
        return;
    }

    if(start_sec > 0) {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -nostdin -hide_banner -loglevel error "
                 "-ss %d -i %s -vn -f s16le -acodec pcm_s16le "
                 "-ac 2 -ar %d - 2>" AUDIO_LOG_PATH " | "
                 AUDIO_PCM_VOLUME_BIN " " AUDIO_VOLUME_CONFIG_PATH
                 " 2>>" AUDIO_LOG_PATH " | "
                 "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
                 "-B %d -F %d - >>" AUDIO_LOG_PATH " 2>&1",
                 start_sec, quoted, AUDIO_PCM_RATE, AUDIO_PCM_RATE,
                 AUDIO_APLAY_BUFFER_US, AUDIO_APLAY_PERIOD_US);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -nostdin -hide_banner -loglevel error -i %s "
                 "-vn -f s16le -acodec pcm_s16le -ac 2 -ar %d - "
                 "2>" AUDIO_LOG_PATH " | "
                 AUDIO_PCM_VOLUME_BIN " " AUDIO_VOLUME_CONFIG_PATH
                 " 2>>" AUDIO_LOG_PATH " | "
                 "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
                 "-B %d -F %d - >>" AUDIO_LOG_PATH " 2>&1",
                 quoted, AUDIO_PCM_RATE, AUDIO_PCM_RATE,
                 AUDIO_APLAY_BUFFER_US, AUDIO_APLAY_PERIOD_US);
    }
    if(audio_start_command(cmd, tracks[index].title, tracks[index].meta) == 0) {
        player_is_radio = 0;
        player_is_recording_file = 0;
        if(start_sec > 0) {
            player_start_us -= (uint64_t)start_sec * 1000000ULL;
        }
    }
    audio_debug_log("LOCAL_START index=%d seek_sec=%d duration=%d title=\"%s\"",
                    index, start_sec, player_duration_sec, tracks[index].title);
}

static void audio_start_local(int index)
{
    audio_start_local_at(index, 0);
}

static void audio_start_radio(int index, const char *name, const char *url)
{
    char quoted[AUDIO_PATH_MAX * 2];
    char cmd[AUDIO_PATH_MAX * 4];
    char net[96];

    if(!url || !name) {
        return;
    }
    if(!audio_network_online(net, sizeof(net))) {
        audio_set_status("Network offline");
        return;
    }
    if(audio_shell_quote(url, quoted, sizeof(quoted)) != 0) {
        audio_set_status("URL error");
        return;
    }

    selected_stream = index;
    player_duration_sec = 0;
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -nostdin -hide_banner -loglevel error "
             "-reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5 "
             "-i %s -vn -f s16le -acodec pcm_s16le -ac 2 -ar %d - "
             "2>" AUDIO_LOG_PATH " | "
             AUDIO_PCM_VOLUME_BIN " " AUDIO_VOLUME_CONFIG_PATH
             " 2>>" AUDIO_LOG_PATH " | "
             "aplay -q -D default -t raw -f S16_LE -c 2 -r %d "
             "-B %d -F %d - >>" AUDIO_LOG_PATH " 2>&1",
             quoted, AUDIO_PCM_RATE, AUDIO_PCM_RATE, AUDIO_APLAY_BUFFER_US,
             AUDIO_APLAY_PERIOD_US);
    if(audio_start_command(cmd, name, "HTTP MP3") == 0) {
        player_is_radio = 1;
        player_is_recording_file = 0;
    }
}

static void audio_stop(void)
{
    audio_terminate_process();
    player_duration_sec = 0;
    audio_set_status("Stopped");
}

static void recorder_format_elapsed(int seconds, char *buf, size_t len)
{
    if(seconds < 0) {
        seconds = 0;
    }
    snprintf(buf, len, "%02d:%02d", seconds / 60, seconds % 60);
}

static int recorder_elapsed_sec(void)
{
    if(recorder_pid <= 0 || recorder_start_us == 0) {
        return recorder_last_duration_sec;
    }
    return (int)((ui_monotonic_us() - recorder_start_us) / 1000000ULL);
}

static int recorder_prepare_capture(void)
{
    char device[128];
    int rc;

    if(!audio_read_card_status(device, sizeof(device))) {
        audio_set_status(device);
        return 0;
    }
    if(!audio_has_command("/usr/bin/arecord")) {
        audio_set_status("arecord missing");
        return 0;
    }
    rc = system("mkdir -p " AUDIO_RECORD_DIR " >/dev/null 2>&1");
    if(rc != 0) {
        audio_set_status("Record dir failed");
        return 0;
    }
    return 1;
}

static void recorder_make_path(char *path, size_t len)
{
    time_t now;
    struct tm tm_now;

    now = time(NULL);
    localtime_r(&now, &tm_now);
    snprintf(path, len,
             AUDIO_RECORD_DIR "/REC_%04d%02d%02d_%02d%02d%02d.wav",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
}

static void recorder_ensure_input_route(const char *owner)
{
    if(recorder_input_route_active) {
        return;
    }

    ui_audio_input_route_enter(owner ? owner : "Recorder");
    recorder_input_route_active = 1;
    recorder_playback_route_suspended = 0;
    audio_debug_log("REC_INPUT_ROUTE_ENTER owner=\"%s\" output_now=%s",
                    owner ? owner : "Recorder",
                    ui_audio_output_is_external() ? "external" : "headphones");
}

static void recorder_suspend_input_route_for_playback(void)
{
    if(!recorder_input_route_active) {
        return;
    }

    ui_audio_input_route_leave("Recorder playback");
    recorder_input_route_active = 0;
    recorder_playback_route_suspended = 1;
    audio_debug_log("REC_INPUT_ROUTE_SUSPEND_FOR_PLAYBACK output_now=%s",
                    ui_audio_output_is_external() ? "external" : "headphones");
}

static void recorder_restore_input_route_after_playback(void)
{
    if(!recorder_playback_route_suspended || active_audio_page != PAGE_RECORDER ||
       player_pid > 0 || recorder_pid > 0) {
        return;
    }

    recorder_ensure_input_route("Recorder restore");
}

static void recorder_start(void)
{
    char path[AUDIO_PATH_MAX];
    char quoted[AUDIO_PATH_MAX * 2];
    char cmd[AUDIO_PATH_MAX * 4];
    pid_t pid;

    if(recorder_pid > 0) {
        return;
    }
    if(!recorder_prepare_capture()) {
        return;
    }

    audio_terminate_process();
    recorder_ensure_input_route("Recorder start");
    recorder_make_path(path, sizeof(path));
    if(audio_shell_quote(path, quoted, sizeof(quoted)) != 0) {
        audio_set_status("Path error");
        return;
    }

    snprintf(cmd, sizeof(cmd),
             "arecord -q -D default -f S16_LE -c 1 -r %d -t wav %s "
             ">" AUDIO_RECORDER_LOG_PATH " 2>&1",
             AUDIO_PCM_RATE, quoted);

    pid = fork();
    if(pid < 0) {
        audio_set_status("fork failed");
        return;
    }
    if(pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    setpgid(pid, pid);
    recorder_pid = pid;
    recorder_start_us = ui_monotonic_us();
    recorder_last_duration_sec = 0;
    snprintf(current_record_path, sizeof(current_record_path), "%s", path);
    audio_set_status("Recording");
    audio_debug_log("REC_START path=\"%s\"", path);
}

static void recorder_stop(void)
{
    int status = 0;

    if(recorder_pid <= 0) {
        return;
    }

    kill(-recorder_pid, SIGINT);
    for(int i = 0; i < 30; i++) {
        if(waitpid(recorder_pid, &status, WNOHANG) == recorder_pid) {
            recorder_last_duration_sec = recorder_elapsed_sec();
            recorder_pid = -1;
            recorder_start_us = 0;
            audio_set_status("Saved");
            audio_debug_log("REC_STOP path=\"%s\" duration=%d size=%ld",
                            current_record_path, recorder_last_duration_sec,
                            audio_file_size(current_record_path));
            recorder_scan_recordings();
            recorder_rebuild_list_overlay();
            return;
        }
        usleep(20000);
    }

    kill(-recorder_pid, SIGTERM);
    waitpid(recorder_pid, &status, 0);
    recorder_last_duration_sec = recorder_elapsed_sec();
    recorder_pid = -1;
    recorder_start_us = 0;
    audio_set_status("Saved");
    recorder_scan_recordings();
    recorder_rebuild_list_overlay();
}

void ui_audio_stop_for_exclusive_app(const char *reason)
{
    if(recorder_pid > 0) {
        recorder_stop();
    }
    if(player_pid > 0) {
        audio_terminate_process();
    }

    player_duration_sec = 0;
    music_progress_dragging = 0;
    audio_set_status(reason && reason[0] ? reason : "Stopped");
    audio_debug_log("EXCLUSIVE_AUDIO_STOP reason=\"%s\"", reason ? reason : "");
}

static void recorder_poll_process(void)
{
    int status;
    pid_t rc;

    if(recorder_pid <= 0) {
        return;
    }

    rc = waitpid(recorder_pid, &status, WNOHANG);
    if(rc == 0) {
        return;
    }
    if(rc == recorder_pid) {
        recorder_last_duration_sec = recorder_elapsed_sec();
        recorder_pid = -1;
        recorder_start_us = 0;
        audio_set_status(WIFEXITED(status) && WEXITSTATUS(status) == 0 ?
                         "Saved" : "Record stopped");
        audio_debug_log("REC_EXIT status=%d path=\"%s\" size=%ld",
                        status, current_record_path,
                        audio_file_size(current_record_path));
        recorder_scan_recordings();
        recorder_rebuild_list_overlay();
    }
}

static void recorder_start_playback(int index)
{
    char quoted[AUDIO_PATH_MAX * 2];
    char cmd[AUDIO_PATH_MAX * 3];

    if(index < 0 || index >= recording_count) {
        audio_set_status("No recording");
        return;
    }
    if(recorder_pid > 0) {
        audio_set_status("Stop recording first");
        return;
    }
    if(!audio_has_command("/usr/bin/aplay")) {
        audio_set_status("aplay missing");
        return;
    }
    if(audio_shell_quote(recordings[index].path, quoted, sizeof(quoted)) != 0) {
        audio_set_status("Path error");
        return;
    }

    selected_recording = index;
    snprintf(cmd, sizeof(cmd),
             "aplay -q -D default %s >>" AUDIO_RECORDER_LOG_PATH " 2>&1",
             quoted);
    recorder_suspend_input_route_for_playback();
    if(audio_start_command(cmd, recordings[index].title,
                           recordings[index].meta) == 0) {
        player_is_radio = 0;
        player_is_recording_file = 1;
        snprintf(player_local_path, sizeof(player_local_path), "%s",
                 recordings[index].path);
        player_duration_sec = audio_probe_path_duration(recordings[index].path);
        audio_set_status("Playing recording");
    } else if(recorder_playback_route_suspended) {
        recorder_restore_input_route_after_playback();
    }
}

static void audio_style_round_button(lv_obj_t *btn, int size, uint32_t bg)
{
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2D3744), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 8);
}

static lv_obj_t *audio_icon_button(lv_obj_t *parent, int x, int y, int size,
                                   const char *symbol, uint32_t bg,
                                   lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_t *icon;

    lv_obj_set_pos(btn, x, y);
    audio_style_round_button(btn, size, bg);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    icon = ui_label(btn, symbol, size > 70 ? &lv_font_montserrat_28 :
                    &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_center(icon);
    ui_make_click_forwarder(icon);
    return icon;
}

static void audio_create_volume_control(lv_obj_t *parent, int x, int y, int w,
                                        lv_obj_t **slider_out,
                                        lv_obj_t **label_out)
{
    lv_obj_t *icon;
    lv_obj_t *slider;
    lv_obj_t *value;

    icon = ui_label(parent, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_22,
                    0x3DA5FF);
    lv_obj_set_pos(icon, x, y + 2);

    slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, x + 42, y + 9);
    lv_obj_set_size(slider, w - 114, 18);
    lv_slider_set_range(slider, AUDIO_VOLUME_MIN, AUDIO_VOLUME_MAX);
    lv_slider_set_value(slider, audio_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A3037), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3DA5FF),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xF2F5F8), LV_PART_KNOB);
    lv_obj_set_style_height(slider, 18, LV_PART_MAIN);
    lv_obj_set_style_width(slider, 24, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 24, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, audio_volume_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, audio_volume_event_cb, LV_EVENT_RELEASED,
                        NULL);

    value = ui_label(parent, "0%", &lv_font_montserrat_16, 0xF2F5F8);
    lv_obj_set_width(value, 58);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(value, x + w - 58, y + 2);

    if(slider_out) {
        *slider_out = slider;
    }
    if(label_out) {
        *label_out = value;
    }
}

static lv_obj_t *audio_create_stack_info_row(lv_obj_t *parent, int y,
                                             const char *name,
                                             const char *value,
                                             uint32_t value_color,
                                             int width)
{
    lv_obj_t *left;
    lv_obj_t *right;

    if(width <= 0) {
        width = lv_obj_get_width(parent) - 32;
    }
    if(width < 220) {
        width = 220;
    }

    left = ui_label(parent, name, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(left, width);
    lv_label_set_long_mode(left, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(left, 0, y);

    right = ui_label(parent, value, &lv_font_montserrat_18, value_color);
    lv_obj_set_width(right, width);
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(right, 0, y + 22);
    return right;
}

static void music_mode_button_cb(lv_event_t *event)
{
    music_play_mode = (music_play_mode_t)(intptr_t)lv_event_get_user_data(event);
    music_refresh_ui();
}

static lv_obj_t *music_create_mode_button(lv_obj_t *parent, int x, int y,
                                          const char *symbol,
                                          const char *badge,
                                          music_play_mode_t mode)
{
    lv_obj_t *btn;
    lv_obj_t *icon;

    btn = lv_obj_create(parent);
    lv_obj_set_pos(btn, x, y);
    audio_style_round_button(btn, 64, 0x242C36);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2A3037), 0);
    lv_obj_add_event_cb(btn, music_mode_button_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)mode);

    icon = ui_label(btn, symbol, &lv_font_montserrat_22, 0xF2F5F8);
    lv_obj_center(icon);
    ui_make_click_forwarder(icon);

    if(badge && badge[0]) {
        lv_obj_t *small = ui_label(btn, badge, &lv_font_montserrat_12,
                                   0xF2F5F8);
        lv_obj_align(small, LV_ALIGN_TOP_RIGHT, -14, 10);
        ui_make_click_forwarder(small);
    }

    music_mode_btn[mode] = btn;
    return btn;
}

static void music_info_open_cb(lv_event_t *event)
{
    (void)event;

    if(music_info_overlay) {
        lv_obj_clear_flag(music_info_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(music_info_overlay);
        music_refresh_ui();
    }
}

static void music_info_close_cb(lv_event_t *event)
{
    (void)event;

    if(music_info_overlay) {
        lv_obj_add_flag(music_info_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void music_refresh_ui(void)
{
    char device[128];
    char count[48];
    char duration[24];
    const char *display_title = player_title;
    const char *display_meta = player_source;
    int local_active;
    int radio_active;
    int has_local_selection;

    audio_poll_process();
    local_active = audio_is_local_playback();
    radio_active = audio_is_radio_playback();
    has_local_selection = selected_track >= 0 && selected_track < track_count;

    if(!local_active && has_local_selection) {
        display_title = tracks[selected_track].title;
        display_meta = tracks[selected_track].meta;
    } else if(!local_active && !has_local_selection) {
        display_title = ui_tr("No music");
        display_meta = "/root/music";
    }
    if(music_title_label) {
        lv_label_set_text(music_title_label, display_title);
    }
    if(music_meta_label) {
        lv_label_set_text(music_meta_label, display_meta);
    }
    if(music_info_title_label) {
        lv_label_set_text(music_info_title_label, display_title);
    }
    if(music_info_duration_label) {
        if(player_duration_sec > 0) {
            audio_format_time(player_duration_sec, duration, sizeof(duration));
            lv_label_set_text(music_info_duration_label, duration);
        } else {
            lv_label_set_text(music_info_duration_label, "--");
        }
    }
    if(music_info_path_label) {
        lv_label_set_text(music_info_path_label,
                          selected_track >= 0 && selected_track < track_count ?
                          tracks[selected_track].path : "--");
    }
    if(music_state_label) {
        lv_label_set_text(music_state_label,
                          radio_active ? ui_tr("Radio playing") :
                          ui_tr(player_status));
        lv_obj_set_style_text_color(music_state_label,
                                    local_active && !player_paused ?
                                    lv_color_hex(0x25C281) :
                                    lv_color_hex(0xF5A524), 0);
    }
    if(music_device_label) {
        audio_read_card_status(device, sizeof(device));
        lv_label_set_text(music_device_label, device);
    }
    if(music_count_label) {
        snprintf(count, sizeof(count), "%d %s", track_count, ui_tr("tracks"));
        lv_label_set_text(music_count_label, count);
    }
    if(music_play_label) {
        lv_label_set_text(music_play_label,
                          local_active && !player_paused ?
                          LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    audio_update_volume_widgets();
    music_update_cover();
    music_update_progress();

    for(int i = 0; i < track_count; i++) {
        int selected = i == selected_track;
        if(music_row[i]) {
            lv_obj_set_style_bg_color(music_row[i],
                                      lv_color_hex(selected ? 0x243847 : 0x151B22),
                                      0);
            lv_obj_set_style_border_color(music_row[i],
                                          lv_color_hex(selected ? 0x3DA5FF : 0x25303A),
                                          0);
        }
        if(music_row_icon[i]) {
            lv_label_set_text(music_row_icon[i],
                              selected && local_active && !player_paused ?
                              LV_SYMBOL_PAUSE : LV_SYMBOL_AUDIO);
        }
        if(music_row_meta[i]) {
            lv_obj_set_style_text_color(music_row_meta[i],
                                        lv_color_hex(selected ? 0x3DA5FF : 0x9AA4AF),
                                        0);
        }
    }

    for(int i = 0; i < MUSIC_MODE_COUNT; i++) {
        int selected = i == (int)music_play_mode;
        if(music_mode_btn[i]) {
            lv_obj_set_style_bg_color(music_mode_btn[i],
                                      lv_color_hex(selected ? 0x2563EB : 0x222A33),
                                      0);
            lv_obj_set_style_border_color(music_mode_btn[i],
                                          lv_color_hex(selected ? 0x7DD3FC : 0x2A3037),
                                          0);
        }
    }
}

static void radio_refresh_ui(void)
{
    char net[96];
    char device[128];
    char count[48];
    int radio_active;

    audio_poll_process();
    radio_active = audio_is_radio_playback();
    if(radio_state_label) {
        lv_label_set_text(radio_state_label,
                          audio_is_local_playback() ? ui_tr("Music playing") :
                          ui_tr(player_status));
        lv_obj_set_style_text_color(radio_state_label,
                                    radio_active && !player_paused ?
                                    lv_color_hex(0x25C281) :
                                    lv_color_hex(0xF5A524), 0);
    }
    if(radio_network_label) {
        audio_network_online(net, sizeof(net));
        lv_label_set_text(radio_network_label, net);
    }
    if(radio_device_label) {
        audio_read_card_status(device, sizeof(device));
        lv_label_set_text(radio_device_label, device);
    }
    if(radio_count_label) {
        snprintf(count, sizeof(count), "%d %s", stream_count, ui_tr("streams"));
        lv_label_set_text(radio_count_label, count);
    }
    audio_update_volume_widgets();

    for(int i = 0; i < stream_count; i++) {
        int selected = i == selected_stream;
        if(radio_row[i]) {
            lv_obj_set_style_bg_color(radio_row[i],
                                      lv_color_hex(selected ? 0x1F3A2B : 0x151B22),
                                      0);
            lv_obj_set_style_border_color(radio_row[i],
                                          lv_color_hex(selected ? 0x25C281 : 0x25303A),
                                          0);
        }
        if(radio_row_icon[i]) {
            lv_label_set_text(radio_row_icon[i],
                              selected && radio_active && !player_paused ?
                              LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
    }
}

static void audio_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    recorder_poll_process();
    audio_poll_process();
    if(active_audio_page == PAGE_MUSIC) {
        music_refresh_ui();
    } else if(active_audio_page == PAGE_NET_RADIO) {
        radio_refresh_ui();
    } else if(active_audio_page == PAGE_RECORDER) {
        recorder_refresh_ui();
    } else if(player_pid <= 0 && recorder_pid <= 0 && audio_timer) {
        lv_timer_delete(audio_timer);
        audio_timer = NULL;
    }
}

static void audio_ensure_timer(void)
{
    if(!audio_timer) {
        audio_timer = lv_timer_create(audio_timer_cb, 1000, NULL);
    }
}

static void music_play_cb(lv_event_t *event)
{
    (void)event;

    if(audio_is_local_playback()) {
        audio_toggle_pause();
    } else if(selected_track >= 0) {
        audio_start_local(selected_track);
    } else if(track_count > 0) {
        audio_start_local(0);
    } else {
        audio_set_status("No tracks");
    }
    music_refresh_ui();
}

static void music_prev_cb(lv_event_t *event)
{
    int next;

    (void)event;
    if(track_count <= 0) {
        audio_set_status("No tracks");
        music_refresh_ui();
        return;
    }

    next = music_prev_index_for_mode();
    audio_start_local(next);
    music_refresh_ui();
}

static void music_next_cb(lv_event_t *event)
{
    int next;

    (void)event;
    if(track_count <= 0) {
        audio_set_status("No tracks");
        music_refresh_ui();
        return;
    }

    next = music_next_index_for_mode();
    audio_start_local(next);
    music_refresh_ui();
}

static void music_row_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    audio_start_local(index);
    if(music_list_overlay) {
        lv_obj_add_flag(music_list_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    music_refresh_ui();
}

static void music_list_open_cb(lv_event_t *event)
{
    (void)event;

    if(music_list_overlay) {
        lv_obj_clear_flag(music_list_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(music_list_overlay);
    }
}

static void music_list_close_cb(lv_event_t *event)
{
    (void)event;

    if(music_list_overlay) {
        lv_obj_add_flag(music_list_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void radio_row_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(index < 0 || index >= stream_count) {
        return;
    }
    if(audio_is_radio_playback() && selected_stream == index) {
        audio_toggle_pause();
    } else {
        audio_start_radio(index, streams[index].name, streams[index].url);
    }
    radio_refresh_ui();
}

static void radio_stop_cb(lv_event_t *event)
{
    (void)event;
    if(audio_is_radio_playback()) {
        audio_stop();
    } else {
        audio_set_status("No radio playing");
    }
    radio_refresh_ui();
}

static void radio_custom_submit_cb(const char *text, void *user_data)
{
    (void)user_data;

    snprintf(custom_stream_url, sizeof(custom_stream_url), "%s", text ? text : "");
    ui_trim_text(custom_stream_url);
    if(strncmp(custom_stream_url, "http://", 7) != 0) {
        audio_set_status("HTTP URL required");
        radio_refresh_ui();
        return;
    }
    audio_start_radio(-1, "Custom stream", custom_stream_url);
    radio_refresh_ui();
}

static void radio_custom_cb(lv_event_t *event)
{
    ui_input_dialog_config_t config = {
        .title = "Radio URL",
        .placeholder = "http://host/path",
        .initial_text = custom_stream_url,
        .password_mode = 0,
        .max_length = AUDIO_PATH_MAX - 1,
        .submit_cb = radio_custom_submit_cb,
        .user_data = NULL,
    };

    (void)event;
    ui_input_dialog_open(&config);
}

static lv_obj_t *audio_create_row_ex(lv_obj_t *parent, int y,
                                     const char *left_icon, const char *title,
                                     const char *meta, uint32_t accent,
                                     lv_event_cb_t cb, void *user_data,
                                     int right_reserved, int text_x,
                                     int row_x, int row_w_override,
                                     lv_obj_t **icon_out,
                                     lv_obj_t **meta_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *detail;
    int row_w = row_w_override > 0 ? row_w_override :
                ui_safe_content_width(parent, 488);
    int text_w = row_w - text_x - 24 - right_reserved;

    if(text_w < 120) {
        text_w = row_w - text_x - 12 - right_reserved;
    }
    if(text_w < 80) {
        text_w = 80;
    }

    lv_obj_set_pos(row, row_x, y);
    lv_obj_set_size(row, row_w, 72);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x151B22), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x25303A), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(row, 4);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);

    icon = ui_label(row, left_icon, &lv_font_montserrat_22, accent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, 0);
    ui_make_click_forwarder(icon);

    name = ui_label(row, title, &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_set_width(name, text_w);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, text_x, 13);
    ui_make_click_forwarder(name);

    detail = ui_label(row, meta, &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(detail, text_w);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, text_x, 42);
    ui_make_click_forwarder(detail);

    if(icon_out) {
        *icon_out = icon;
    }
    if(meta_out) {
        *meta_out = detail;
    }
    return row;
}

static lv_obj_t *audio_create_row(lv_obj_t *parent, int y, const char *left_icon,
                                  const char *title, const char *meta,
                                  uint32_t accent, lv_event_cb_t cb,
                                  void *user_data, int right_reserved,
                                  lv_obj_t **icon_out, lv_obj_t **meta_out)
{
    return audio_create_row_ex(parent, y, left_icon, title, meta, accent, cb,
                               user_data, right_reserved, 58, 0, 0, icon_out,
                               meta_out);
}

static int notification_current_index_from_list(char *name, size_t len)
{
    char value[AUDIO_TITLE_MAX];
    int idx;

    if(name && len > 0) {
        name[0] = '\0';
    }

    ui_prefs_get(AUDIO_NOTIFICATION_PREF, value, sizeof(value), "");
    ui_trim_text(value);
    if(strcmp(value, AUDIO_NOTIFICATION_NONE) == 0) {
        if(name && len > 0) {
            snprintf(name, len, "%s", AUDIO_NOTIFICATION_NONE);
        }
        return -1;
    }
    if(notification_name_valid(value)) {
        idx = notification_find_index(value);
        if(idx >= 0) {
            if(name && len > 0) {
                snprintf(name, len, "%s", notification_sounds[idx].title);
            }
            return idx;
        }
    }

    idx = notification_default_index();
    if(idx >= 0 && name && len > 0) {
        snprintf(name, len, "%s", notification_sounds[idx].title);
    }
    return idx;
}

static void notification_style_row(lv_obj_t *row, lv_obj_t *meta,
                                   int selected, const char *default_meta,
                                   uint32_t accent)
{
    if(row) {
        lv_obj_set_style_bg_color(row,
                                  lv_color_hex(selected ? 0x211B32 :
                                               0x151B22), 0);
        lv_obj_set_style_border_color(row,
                                      lv_color_hex(selected ? accent :
                                                   0x25303A), 0);
    }
    if(meta) {
        lv_label_set_text(meta, selected ? ui_tr("Selected") :
                          ui_tr(default_meta ? default_meta : ""));
        lv_obj_set_style_text_color(meta,
                                    lv_color_hex(selected ? accent :
                                                 0x9AA4AF), 0);
    }
}

static void notification_refresh_ui(void)
{
    char name[AUDIO_TITLE_MAX];
    int idx;

    idx = notification_current_index_from_list(name, sizeof(name));
    snprintf(notification_selected, sizeof(notification_selected), "%s",
             idx >= 0 ? notification_sounds[idx].title :
             (strcmp(name, AUDIO_NOTIFICATION_NONE) == 0 ?
              AUDIO_NOTIFICATION_NONE : ""));

    if(notification_current_label) {
        char text[160];

        snprintf(text, sizeof(text), "%s: %s", ui_tr("Current"),
                 idx >= 0 ? notification_sounds[idx].title : ui_tr("Off"));
        lv_label_set_text(notification_current_label, text);
    }
    if(notification_status_label) {
        if(notification_sound_count == 0) {
            lv_label_set_text(notification_status_label,
                              ui_tr("No notification sounds"));
            lv_obj_set_style_text_color(notification_status_label,
                                        lv_color_hex(0xF5A524), 0);
        } else {
            char text[96];

            snprintf(text, sizeof(text), "%d %s", notification_sound_count,
                     ui_tr("sounds"));
            lv_label_set_text(notification_status_label, text);
            lv_obj_set_style_text_color(notification_status_label,
                                        lv_color_hex(0x9AA4AF), 0);
        }
    }

    notification_style_row(notification_row[0], notification_row_meta[0],
                           idx < 0 &&
                           strcmp(notification_selected,
                                  AUDIO_NOTIFICATION_NONE) == 0,
                           "Disable sound", 0x9AA4AF);
    for(int i = 0; i < notification_sound_count; i++) {
        notification_style_row(notification_row[i + 1],
                               notification_row_meta[i + 1],
                               i == idx, notification_sounds[i].meta,
                               0xA78BFA);
    }
}

static void notification_select_cb(lv_event_t *event)
{
    intptr_t raw = (intptr_t)lv_event_get_user_data(event);
    int play_result = 0;

    if(raw < 0) {
        ui_prefs_set(AUDIO_NOTIFICATION_PREF, AUDIO_NOTIFICATION_NONE);
    } else if(raw < notification_sound_count) {
        ui_prefs_set(AUDIO_NOTIFICATION_PREF, notification_sounds[raw].title);
        play_result = audio_play_notification_path(notification_sounds[raw].path);
    }
    notification_refresh_ui();
    if(raw >= 0 && raw < notification_sound_count && notification_status_label) {
        if(play_result != 0) {
            lv_label_set_text(notification_status_label,
                              ui_tr("Playback failed"));
            lv_obj_set_style_text_color(notification_status_label,
                                        lv_color_hex(0xEF4D5A), 0);
        }
    } else if(raw < 0 && notification_status_label) {
        lv_label_set_text(notification_status_label, ui_tr("Off"));
        lv_obj_set_style_text_color(notification_status_label,
                                    lv_color_hex(0x9AA4AF), 0);
    }
    app_request_fast_refresh();
}

void ui_notification_settings_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *title;
    lv_obj_t *hint;
    int x = ui_page_panel_x();
    int w = ui_page_panel_width();
    int content_w;
    int list_x = ui_is_landscape() ? 24 : 16;
    int list_w;
    int y = 0;

    ui_create_header(scr, "Notifications");
    body = ui_scroll_panel(scr, x, ui_page_top_y(154), w, ui_body_height(154));
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(body, 16, 0);
    content_w = ui_safe_content_width(body, 488);
    list_w = content_w - list_x * 2;
    if(list_w < 260) {
        list_w = content_w > 260 ? content_w - list_x : 260;
        list_x = (content_w - list_w) / 2;
    }

    notification_scan_sounds();
    memset(notification_row, 0, sizeof(notification_row));
    memset(notification_row_meta, 0, sizeof(notification_row_meta));

    title = ui_label(body, "Notification sound", &lv_font_montserrat_24,
                     0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, y);
    y += 44;

    notification_current_label = ui_label(body, "--", &lv_font_montserrat_18,
                                          0xA78BFA);
    lv_obj_set_width(notification_current_label, content_w);
    lv_label_set_long_mode(notification_current_label, LV_LABEL_LONG_DOT);
    lv_obj_align(notification_current_label, LV_ALIGN_TOP_LEFT, 0, y);
    y += 34;

    notification_status_label = ui_label(body, "--", &lv_font_montserrat_16,
                                         0x9AA4AF);
    lv_obj_set_width(notification_status_label, content_w);
    lv_label_set_long_mode(notification_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(notification_status_label, LV_ALIGN_TOP_LEFT, 0, y);
    y += 44;

    hint = ui_label(body, "Sounds in /root/notification",
                    &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_width(hint, content_w);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, y);
    y += 42;

    notification_row[0] = audio_create_row_ex(body, y, "OFF", "Off",
                                              "Disable sound", 0x9AA4AF,
                                              notification_select_cb,
                                              (void *)(intptr_t)-1, 0, 82,
                                              list_x, list_w, NULL,
                                              &notification_row_meta[0]);
    y += 84;

    for(int i = 0; i < notification_sound_count; i++) {
        notification_row[i + 1] =
            audio_create_row_ex(body, y, LV_SYMBOL_AUDIO,
                                notification_sounds[i].title,
                                notification_sounds[i].meta, 0xA78BFA,
                                notification_select_cb,
                                (void *)(intptr_t)i, 0, 82,
                                list_x, list_w, NULL,
                                &notification_row_meta[i + 1]);
        y += 84;
    }

    if(notification_sound_count == 0) {
        lv_obj_t *empty = ui_label(body, "No notification sounds",
                                   &lv_font_montserrat_20, 0xF5A524);
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, y + 12);
    }

    notification_refresh_ui();
}

static void recorder_record_cb(lv_event_t *event)
{
    (void)event;

    if(recorder_pid > 0) {
        recorder_stop();
    } else {
        recorder_start();
    }
    recorder_refresh_ui();
}

static void recorder_play_cb(lv_event_t *event)
{
    (void)event;

    if(audio_is_recorder_playback()) {
        audio_toggle_pause();
    } else {
        recorder_start_playback(selected_recording);
    }
    recorder_refresh_ui();
}

static void recorder_list_open_cb(lv_event_t *event)
{
    (void)event;

    recorder_scan_recordings();
    recorder_rebuild_list_overlay();
    if(recorder_list_overlay) {
        lv_obj_clear_flag(recorder_list_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(recorder_list_overlay);
        recorder_refresh_ui();
    }
}

static void recorder_list_close_cb(lv_event_t *event)
{
    (void)event;

    if(recorder_list_overlay) {
        lv_obj_add_flag(recorder_list_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void recorder_row_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(index < 0 || index >= recording_count) {
        return;
    }
    selected_recording = index;
    snprintf(current_record_path, sizeof(current_record_path), "%s",
             recordings[index].path);
    if(recorder_list_overlay) {
        lv_obj_add_flag(recorder_list_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    recorder_start_playback(index);
    recorder_refresh_ui();
}

static void recorder_close_delete_confirm(void)
{
    if(recorder_delete_confirm_overlay &&
       lv_obj_is_valid(recorder_delete_confirm_overlay)) {
        lv_obj_delete(recorder_delete_confirm_overlay);
    }
    recorder_delete_confirm_overlay = NULL;
    recorder_delete_pending_path[0] = '\0';
    recorder_delete_pending_title[0] = '\0';
}

static void recorder_delete_confirm_cancel_cb(lv_event_t *event)
{
    (void)event;
    recorder_close_delete_confirm();
}

static void recorder_delete_path(const char *path)
{
    if(!path || path[0] == '\0') {
        return;
    }

    if(strcmp(current_record_path, path) == 0 && audio_is_recorder_playback()) {
        audio_terminate_process();
    }
    if(unlink(path) == 0) {
        if(strcmp(current_record_path, path) == 0) {
            current_record_path[0] = '\0';
        }
        audio_set_status("Recording deleted");
        audio_debug_log("REC_DELETE path=\"%s\"", path);
    } else {
        audio_set_status("Delete failed");
        audio_debug_log("REC_DELETE_FAILED path=\"%s\" errno=%d", path, errno);
    }

    recorder_scan_recordings();
    recorder_rebuild_list_overlay();
    recorder_refresh_ui();
}

static void recorder_delete_confirm_accept_cb(lv_event_t *event)
{
    char path[AUDIO_PATH_MAX];

    (void)event;
    snprintf(path, sizeof(path), "%s", recorder_delete_pending_path);
    recorder_close_delete_confirm();
    recorder_delete_path(path);
}

static void recorder_open_delete_confirm(int index)
{
    lv_obj_t *dialog;
    lv_obj_t *title;
    lv_obj_t *name;
    lv_obj_t *note;
    lv_obj_t *btn;
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int landscape = ui_is_landscape();
    int dialog_w = landscape ? 560 : 500;
    int dialog_h = landscape ? 246 : 268;
    int pad = 24;
    int button_gap = 18;
    int button_w;
    int button_y;

    if(index < 0 || index >= recording_count) {
        return;
    }

    recorder_close_delete_confirm();
    snprintf(recorder_delete_pending_path, sizeof(recorder_delete_pending_path),
             "%s", recordings[index].path);
    snprintf(recorder_delete_pending_title, sizeof(recorder_delete_pending_title),
             "%s", recordings[index].title);

    if(dialog_w > screen_w - 48) {
        dialog_w = screen_w - 48;
    }
    if(dialog_w < 320) {
        dialog_w = 320;
    }
    if(dialog_h > screen_h - 48) {
        dialog_h = screen_h - 48;
    }
    if(dialog_h < 220) {
        dialog_h = 220;
    }
    button_w = (dialog_w - pad * 2 - button_gap) / 2;
    button_y = dialog_h - pad - 58;

    recorder_delete_confirm_overlay = lv_obj_create(lv_layer_top());
    ui_set_fullscreen(recorder_delete_confirm_overlay);
    lv_obj_set_style_bg_color(recorder_delete_confirm_overlay,
                              lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(recorder_delete_confirm_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(recorder_delete_confirm_overlay, 0, 0);
    lv_obj_set_style_pad_all(recorder_delete_confirm_overlay, 0, 0);
    lv_obj_clear_flag(recorder_delete_confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);

    dialog = ui_panel(recorder_delete_confirm_overlay, 0, 0, dialog_w,
                      dialog_h);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(dialog, 16, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x3A2630), 0);
    lv_obj_set_style_pad_all(dialog, 0, 0);

    title = ui_label(dialog, "Delete recording?", &lv_font_montserrat_22,
                     0xF2F5F8);
    lv_obj_set_pos(title, pad, pad);
    lv_obj_set_width(title, dialog_w - pad * 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    name = ui_label(dialog, recorder_delete_pending_title,
                    &lv_font_montserrat_18, 0xF5A524);
    lv_obj_set_pos(name, pad, pad + 54);
    lv_obj_set_width(name, dialog_w - pad * 2);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    note = ui_label(dialog, "This cannot be undone.", &lv_font_montserrat_16,
                    0x9AA4AF);
    lv_obj_set_pos(note, pad, pad + 92);
    lv_obj_set_width(note, dialog_w - pad * 2);
    lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);

    btn = ui_command_button(dialog, pad, button_y, button_w, "Cancel",
                            0x9AA4AF);
    lv_obj_set_height(btn, 58);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2A3644), 0);
    lv_obj_add_event_cb(btn, recorder_delete_confirm_cancel_cb,
                        LV_EVENT_CLICKED, NULL);

    btn = ui_command_button(dialog, pad + button_w + button_gap, button_y,
                            button_w, "Delete", 0xEF4D5A);
    lv_obj_set_height(btn, 58);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A1D24), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4A2632), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xEF4D5A), 0);
    lv_obj_add_event_cb(btn, recorder_delete_confirm_accept_cb,
                        LV_EVENT_CLICKED, NULL);
    app_request_fast_refresh();
}

static void recorder_delete_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    if(event) {
        lv_event_stop_processing(event);
    }
    recorder_open_delete_confirm(index);
}

static void recorder_layout_list_overlay(void)
{
    int landscape = ui_is_landscape();
    int edge_back = app_edge_back_enabled();
    int screen_w = ui_screen_width();
    int screen_h = ui_screen_height();
    int title_x = edge_back ? 24 : 108;
    int title_y = landscape ? 28 : 82;
    int panel_y = landscape ? 88 : 154;
    int panel_h;
    int panel_w;

    if(edge_back) {
        title_y = landscape ? 24 : 72;
        panel_y = landscape ? 78 : 130;
    }

    if(recorder_list_back && lv_obj_is_valid(recorder_list_back)) {
        if(edge_back) {
            lv_obj_add_flag(recorder_list_back, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(recorder_list_back, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(recorder_list_back, 24, landscape ? 20 : 74);
        }
    }
    if(recorder_list_title && lv_obj_is_valid(recorder_list_title)) {
        lv_obj_set_pos(recorder_list_title, title_x, title_y);
        lv_obj_set_width(recorder_list_title,
                         screen_w - title_x - 24 > 220 ?
                         screen_w - title_x - 24 : 220);
    }
    if(recorder_list_panel && lv_obj_is_valid(recorder_list_panel)) {
        panel_w = screen_w - 48;
        if(panel_w < 320) {
            panel_w = 320;
        }
        panel_h = screen_h - panel_y - 24;
        if(panel_h < 220) {
            panel_h = screen_h > panel_y + 80 ? screen_h - panel_y - 8 : 220;
        }
        lv_obj_set_pos(recorder_list_panel, 24, panel_y);
        lv_obj_set_size(recorder_list_panel, panel_w, panel_h);
        lv_obj_set_scroll_dir(recorder_list_panel, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(recorder_list_panel, LV_SCROLLBAR_MODE_AUTO);
    }
}

static void recorder_rebuild_list_overlay(void)
{
    lv_obj_t *count;

    if(!recorder_list_panel || !lv_obj_is_valid(recorder_list_panel)) {
        return;
    }

    lv_obj_clean(recorder_list_panel);
    recorder_layout_list_overlay();
    memset(recorder_row, 0, sizeof(recorder_row));
    memset(recorder_row_icon, 0, sizeof(recorder_row_icon));
    memset(recorder_row_meta, 0, sizeof(recorder_row_meta));

    if(recording_count == 0) {
        lv_obj_t *empty = ui_label(recorder_list_panel, "No recordings",
                                   &lv_font_montserrat_22, 0xF5A524);
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, 8);
        empty = ui_label(recorder_list_panel, AUDIO_RECORD_DIR,
                         &lv_font_montserrat_18, 0x9AA4AF);
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, 50);
        return;
    }

    char text[48];
    snprintf(text, sizeof(text), "%d recordings", recording_count);
    count = ui_label(recorder_list_panel, text, &lv_font_montserrat_16,
                     0x9AA4AF);
    lv_obj_align(count, LV_ALIGN_TOP_LEFT, 0, 0);

    for(int i = 0; i < recording_count; i++) {
        lv_obj_t *del;
        lv_obj_t *del_icon;
        int row_w;

        recorder_row[i] = audio_create_row(recorder_list_panel, 42 + i * 84,
                                           AUDIO_ICON_REC,
                                           recordings[i].title,
                                           recordings[i].meta,
                                           0xEF4D5A,
                                           recorder_row_cb,
                                           (void *)(intptr_t)i,
                                           86,
                                           &recorder_row_icon[i],
                                           &recorder_row_meta[i]);
        row_w = lv_obj_get_width(recorder_row[i]);
        if(row_w <= 0) {
            row_w = ui_fit_width(recorder_list_panel, 0, 488);
        }
        del = lv_obj_create(recorder_row[i]);
        lv_obj_set_pos(del, row_w - 72, 14);
        lv_obj_set_size(del, 62, 44);
        lv_obj_set_style_radius(del, 8, 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(0x2A1D24), 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(0x4A2632),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(del, 1, 0);
        lv_obj_set_style_border_color(del, lv_color_hex(0xEF4D5A), 0);
        lv_obj_clear_flag(del, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(del, 8);
        lv_obj_add_event_cb(del, recorder_delete_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_move_foreground(del);
        del_icon = ui_label(del, "Del", &lv_font_montserrat_16,
                            0xEF4D5A);
        lv_obj_center(del_icon);
        ui_make_click_forwarder(del_icon);
    }
}

static void recorder_create_list_overlay(lv_obj_t *scr)
{
    lv_obj_t *back_icon;

    recorder_list_overlay = lv_obj_create(scr);
    lv_obj_set_pos(recorder_list_overlay, 0, 0);
    ui_set_fullscreen(recorder_list_overlay);
    lv_obj_set_style_bg_color(recorder_list_overlay, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(recorder_list_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(recorder_list_overlay, 0, 0);
    lv_obj_set_style_radius(recorder_list_overlay, 0, 0);
    lv_obj_set_style_pad_all(recorder_list_overlay, 0, 0);
    lv_obj_clear_flag(recorder_list_overlay, LV_OBJ_FLAG_SCROLLABLE);

    recorder_list_back = lv_obj_create(recorder_list_overlay);
    audio_style_round_button(recorder_list_back, 58, 0x222832);
    lv_obj_set_style_radius(recorder_list_back, 8, 0);
    lv_obj_add_event_cb(recorder_list_back, recorder_list_close_cb,
                        LV_EVENT_CLICKED, NULL);
    back_icon = ui_label(recorder_list_back, LV_SYMBOL_LEFT,
                         &lv_font_montserrat_24,
                         0xF2F5F8);
    lv_obj_center(back_icon);
    ui_make_click_forwarder(back_icon);

    recorder_list_title = ui_label(recorder_list_overlay, "Recordings",
                                   &lv_font_montserrat_28, 0xF2F5F8);
    lv_label_set_long_mode(recorder_list_title, LV_LABEL_LONG_DOT);

    recorder_list_panel = ui_panel(recorder_list_overlay, 24, 154, 520, 1038);
    lv_obj_set_style_bg_color(recorder_list_panel, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(recorder_list_panel, 16, 0);
    lv_obj_add_flag(recorder_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(recorder_list_panel, LV_DIR_VER);
    recorder_layout_list_overlay();
    recorder_rebuild_list_overlay();

    lv_obj_add_flag(recorder_list_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void recorder_refresh_ui(void)
{
    char elapsed[24];
    char status[160];
    char device[128];
    char count[48];
    const char *file_label = "No recording selected";
    int playing;

    recorder_poll_process();
    audio_poll_process();
    recorder_restore_input_route_after_playback();
    playing = audio_is_recorder_playback();

    if(recorder_pid > 0) {
        recorder_format_elapsed(recorder_elapsed_sec(), elapsed, sizeof(elapsed));
    } else if(player_duration_sec > 0 && playing) {
        recorder_format_elapsed(audio_current_elapsed_sec(), elapsed,
                                sizeof(elapsed));
    } else if(selected_recording >= 0 && selected_recording < recording_count) {
        recorder_format_elapsed(audio_probe_path_duration(
                                    recordings[selected_recording].path),
                                elapsed, sizeof(elapsed));
    } else {
        snprintf(elapsed, sizeof(elapsed), "00:00");
    }

    if(selected_recording >= 0 && selected_recording < recording_count) {
        file_label = recordings[selected_recording].title;
    } else if(current_record_path[0]) {
        file_label = audio_basename(current_record_path);
    }

    if(recorder_timer_label) {
        lv_label_set_text(recorder_timer_label, elapsed);
    }
    if(recorder_status_label) {
        snprintf(status, sizeof(status), "%s%s",
                 recorder_pid > 0 ? ui_tr("Recording") :
                 playing ? (player_paused ? ui_tr("Paused") : ui_tr("Playing")) :
                 ui_tr(player_status),
                 recorder_pid > 0 ? "  WAV 48kHz mono" : "");
        lv_label_set_text(recorder_status_label, status);
        lv_obj_set_style_text_color(recorder_status_label,
                                    lv_color_hex(recorder_pid > 0 ? 0xEF4D5A :
                                                 playing ? 0x25C281 :
                                                 0xF5A524), 0);
    }
    if(recorder_file_label) {
        lv_label_set_text(recorder_file_label, file_label);
    }
    if(recorder_device_label) {
        audio_read_card_status(device, sizeof(device));
        lv_label_set_text(recorder_device_label, device);
    }
    if(recorder_count_label) {
        snprintf(count, sizeof(count), "%d %s", recording_count, ui_tr("files"));
        lv_label_set_text(recorder_count_label, count);
    }
    if(recorder_record_label) {
        lv_label_set_text(recorder_record_label,
                          recorder_pid > 0 ? AUDIO_ICON_STOP : AUDIO_ICON_MIC);
    }
    if(recorder_play_label) {
        lv_label_set_text(recorder_play_label,
                          playing && !player_paused ? LV_SYMBOL_PAUSE :
                          LV_SYMBOL_PLAY);
    }

    for(int i = 0; i < recording_count; i++) {
        int selected = i == selected_recording;

        if(recorder_row[i]) {
            lv_obj_set_style_bg_color(recorder_row[i],
                                      lv_color_hex(selected ? 0x3A1F29 :
                                                   0x151B22), 0);
            lv_obj_set_style_border_color(recorder_row[i],
                                          lv_color_hex(selected ? 0xEF4D5A :
                                                       0x25303A), 0);
        }
        if(recorder_row_icon[i]) {
            lv_label_set_text(recorder_row_icon[i],
                              selected && playing && !player_paused ?
                              LV_SYMBOL_PAUSE : AUDIO_ICON_REC);
        }
        if(recorder_row_meta[i]) {
            lv_obj_set_style_text_color(recorder_row_meta[i],
                                        lv_color_hex(selected ? 0xEF4D5A :
                                                     0x9AA4AF), 0);
        }
    }
}

static void music_create_list_overlay(lv_obj_t *scr)
{
    lv_obj_t *back;
    lv_obj_t *back_icon;
    lv_obj_t *title;
    lv_obj_t *list;
    lv_obj_t *count;

    music_list_overlay = lv_obj_create(scr);
    lv_obj_set_pos(music_list_overlay, 0, 0);
    ui_set_fullscreen(music_list_overlay);
    lv_obj_set_style_bg_color(music_list_overlay, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(music_list_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_list_overlay, 0, 0);
    lv_obj_set_style_radius(music_list_overlay, 0, 0);
    lv_obj_set_style_pad_all(music_list_overlay, 0, 0);
    lv_obj_clear_flag(music_list_overlay, LV_OBJ_FLAG_SCROLLABLE);

    back = lv_obj_create(music_list_overlay);
    lv_obj_set_pos(back, 24, 74);
    lv_obj_set_size(back, 64, 54);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x222832), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2D3744), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(back, 2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x2A3037), 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 8);
    lv_obj_add_event_cb(back, music_list_close_cb, LV_EVENT_CLICKED, NULL);
    back_icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_24,
                         0xF2F5F8);
    lv_obj_center(back_icon);
    ui_make_click_forwarder(back_icon);

    title = ui_label(music_list_overlay, "Music list", &lv_font_montserrat_28,
                     0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 108, 82);

    list = ui_panel(music_list_overlay, 24, 154, 520, 1038);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(list, 16, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if(track_count == 0) {
        lv_obj_t *empty = ui_label(list, "No tracks found",
                                   &lv_font_montserrat_22, 0xF5A524);
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, 8);
        empty = ui_label(list, "/root/music", &lv_font_montserrat_18,
                         0x9AA4AF);
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, 50);
    } else {
        char text[48];

        snprintf(text, sizeof(text), "%d tracks", track_count);
        count = ui_label(list, text, &lv_font_montserrat_16, 0x9AA4AF);
        lv_obj_align(count, LV_ALIGN_TOP_LEFT, 0, 0);
        for(int i = 0; i < track_count; i++) {
            music_row[i] = audio_create_row(list, 42 + i * 84,
                                            LV_SYMBOL_AUDIO, tracks[i].title,
                                            tracks[i].meta, 0x3DA5FF,
                                            music_row_cb,
                                            (void *)(intptr_t)i,
                                            0,
                                            &music_row_icon[i],
                                            &music_row_meta[i]);
        }
    }

    lv_obj_add_flag(music_list_overlay, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *music_info_value_row(lv_obj_t *parent, int y,
                                      const char *name, const char *value,
                                      uint32_t color)
{
    lv_obj_t *left;
    lv_obj_t *right;

    left = ui_label(parent, name, &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_set_pos(left, 0, y);

    right = ui_label(parent, value, &lv_font_montserrat_18, color);
    lv_obj_set_width(right, ui_inner_width());
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(right, 0, y + 30);
    return right;
}

static void music_create_info_overlay(lv_obj_t *scr)
{
    lv_obj_t *back;
    lv_obj_t *back_icon;
    lv_obj_t *title;
    lv_obj_t *body;

    music_info_overlay = lv_obj_create(scr);
    lv_obj_set_pos(music_info_overlay, 0, 0);
    ui_set_fullscreen(music_info_overlay);
    lv_obj_set_style_bg_color(music_info_overlay, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(music_info_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_info_overlay, 0, 0);
    lv_obj_set_style_radius(music_info_overlay, 0, 0);
    lv_obj_set_style_pad_all(music_info_overlay, 0, 0);
    lv_obj_clear_flag(music_info_overlay, LV_OBJ_FLAG_SCROLLABLE);

    back = lv_obj_create(music_info_overlay);
    lv_obj_set_pos(back, 24, 74);
    audio_style_round_button(back, 58, 0x222832);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, music_info_close_cb, LV_EVENT_CLICKED, NULL);
    back_icon = ui_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_24,
                         0xF2F5F8);
    lv_obj_center(back_icon);
    ui_make_click_forwarder(back_icon);

    title = ui_label(music_info_overlay, "Song info", &lv_font_montserrat_28,
                     0xF2F5F8);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 108, 82);

    body = ui_panel(music_info_overlay, 24, 154, 520, 690);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x151B22), 0);
    lv_obj_set_style_pad_all(body, 16, 0);

    music_info_title_label = music_info_value_row(body, 0, "Title", "--",
                                                  0xF2F5F8);
    music_meta_label = music_info_value_row(body, 88, "Format", "--",
                                            0x3DA5FF);
    music_info_duration_label = music_info_value_row(body, 176, "Duration",
                                                     "--", 0x25C281);
    music_state_label = music_info_value_row(body, 264, "State", "--",
                                             0xF5A524);
    music_device_label = music_info_value_row(body, 352, "Device", "--",
                                              0xF2F5F8);
    music_info_path_label = music_info_value_row(body, 440, "Path", "--",
                                                 0x9AA4AF);

    lv_obj_add_flag(music_info_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_music_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *cover;
    lv_obj_t *cover_icon;
    lv_obj_t *cover_text;
    lv_obj_t *progress_panel;
    int landscape = ui_is_landscape();
    int screen_w = ui_screen_width();
    int body_h = ui_body_height(144);
    int cover_size = landscape ? body_h - 48 : AUDIO_COVER_SIZE;
    int cover_x;
    int cover_img_size;
    int control_x;
    int control_w;
    int title_y = landscape ? 24 : 540;
    int controls_y = landscape ? 92 : 656;
    int progress_y = landscape ? 206 : 764;
    int secondary_y = landscape ? 296 : 888;
    int volume_y = landscape ? body_h - 52 : 990;
    int center_x;
    int group_gap = landscape ? 12 : 18;
    int group_w;
    int group_x;
    int volume_x;
    int volume_w;

    if(landscape) {
        if(cover_size > screen_w / 2 - 80) {
            cover_size = screen_w / 2 - 80;
        }
        if(cover_size > 440) {
            cover_size = 440;
        }
        if(cover_size > body_h - 40) {
            cover_size = body_h - 40;
        }
        if(cover_size < 320) {
            cover_size = 320;
        }
    }
    cover_img_size = landscape ? cover_size : AUDIO_COVER_SIZE;
    cover_x = landscape ? 24 : (screen_w - cover_size) / 2;
    if(cover_x < 24) {
        cover_x = 24;
    }
    control_x = landscape ? 24 + cover_size + 24 : 34;
    control_w = landscape ? screen_w - control_x - 24 : 500;
    if(control_w < 340) {
        control_w = 340;
    }
    if(landscape) {
        if(volume_y > 424) {
            volume_y = 424;
        }
        if(volume_y < 374) {
            volume_y = 374;
        }
    }
    center_x = control_x + control_w / 2;
    group_w = 64 * 5 + group_gap * 4;
    if(group_w > control_w) {
        group_gap = 6;
        group_w = 64 * 5 + group_gap * 4;
    }
    group_x = control_x + (control_w - group_w) / 2;
    if(group_x < control_x) {
        group_x = control_x;
    }
    volume_w = landscape ? 300 : control_w - 76;
    if(landscape && volume_w > control_w - 96) {
        volume_w = control_w - 96;
    }
    if(volume_w < 260) {
        volume_w = 260;
    }
    volume_x = landscape ? center_x - volume_w / 2 : control_x + 38;
    if(volume_x < control_x) {
        volume_x = control_x;
    }

    active_audio_page = PAGE_MUSIC;
    audio_scan_tracks();
    audio_load_volume_once();
    if(player_pid <= 0) {
        snprintf(player_title, sizeof(player_title), "%s",
                 track_count > 0 ? tracks[selected_track].title : "No music");
        snprintf(player_source, sizeof(player_source), "%s", "/root/music");
        audio_set_status("Ready");
    }

    ui_create_header(scr, "Music");

    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    cover = ui_panel(body, cover_x, landscape ? 20 : 20, cover_size,
                     cover_size);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x182331), 0);
    lv_obj_set_style_border_color(cover, lv_color_hex(0x2A3B4F), 0);
    lv_obj_set_style_pad_all(cover, 0, 0);

    music_cover_image = lv_image_create(cover);
    lv_obj_set_size(music_cover_image, cover_img_size, cover_img_size);
    lv_obj_center(music_cover_image);
    lv_image_set_scale(music_cover_image,
                       landscape ? (uint32_t)((cover_img_size * LV_SCALE_NONE) /
                                             AUDIO_COVER_SIZE) : LV_SCALE_NONE);
    lv_image_set_inner_align(music_cover_image, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(music_cover_image, LV_OBJ_FLAG_HIDDEN);

    music_cover_placeholder = lv_obj_create(cover);
    lv_obj_set_size(music_cover_placeholder, cover_img_size, cover_img_size);
    lv_obj_center(music_cover_placeholder);
    lv_obj_set_style_bg_color(music_cover_placeholder, lv_color_hex(0x17202B),
                              0);
    lv_obj_set_style_bg_opa(music_cover_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(music_cover_placeholder, 8, 0);
    lv_obj_set_style_border_width(music_cover_placeholder, 0, 0);
    lv_obj_clear_flag(music_cover_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    cover_icon = ui_label(music_cover_placeholder, LV_SYMBOL_AUDIO,
                          &lv_font_montserrat_32, 0x3DA5FF);
    lv_obj_align(cover_icon, LV_ALIGN_CENTER, 0, -28);
    cover_text = ui_label(music_cover_placeholder, "Album", &lv_font_montserrat_22,
                          0xD3DAE3);
    lv_obj_align(cover_text, LV_ALIGN_CENTER, 0, 28);

    music_title_label = ui_label(body, player_title, &lv_font_montserrat_26,
                                 0xF2F5F8);
    lv_obj_set_width(music_title_label, control_w);
    lv_label_set_long_mode(music_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(music_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(music_title_label, control_x, title_y);

    audio_icon_button(body, landscape ? center_x - 170 : control_x + 74,
                      controls_y, 76, LV_SYMBOL_PREV, 0x242C36,
                      music_prev_cb, NULL);
    music_play_label = audio_icon_button(body,
                                         landscape ? center_x - 52 :
                                         control_x + 198,
                                         controls_y - 20, 104,
                                         LV_SYMBOL_PLAY, 0x2563EB,
                                         music_play_cb, NULL);
    audio_icon_button(body, landscape ? center_x + 94 : control_x + 350,
                      controls_y, 76, LV_SYMBOL_NEXT, 0x242C36,
                      music_next_cb, NULL);

    progress_panel = ui_panel(body, control_x, progress_y, control_w, 66);
    lv_obj_set_style_bg_opa(progress_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_panel, 0, 0);
    lv_obj_set_style_radius(progress_panel, 0, 0);
    lv_obj_set_style_pad_all(progress_panel, 0, 0);
    music_progress_label = ui_label(progress_panel, "--:-- / --:--",
                                    &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_pos(music_progress_label, 18, 8);
    music_progress_slider = lv_slider_create(progress_panel);
    lv_obj_set_pos(music_progress_slider, 18, 38);
    lv_obj_set_size(music_progress_slider, control_w - 36, 14);
    lv_slider_set_range(music_progress_slider, 0, 100);
    lv_slider_set_value(music_progress_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(music_progress_slider, lv_color_hex(0x2A3037),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(music_progress_slider, lv_color_hex(0x3DA5FF),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(music_progress_slider, lv_color_hex(0xF2F5F8),
                              LV_PART_KNOB);
    lv_obj_set_style_height(music_progress_slider, 14, LV_PART_MAIN);
    lv_obj_set_style_width(music_progress_slider, 30, LV_PART_KNOB);
    lv_obj_set_style_height(music_progress_slider, 30, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(music_progress_slider, LV_OPA_TRANSP,
                            LV_PART_KNOB);
    lv_obj_set_style_border_width(music_progress_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(music_progress_slider, music_progress_event_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(music_progress_slider, music_progress_event_cb,
                        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(music_progress_slider, music_progress_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(music_progress_slider, music_progress_event_cb,
                        LV_EVENT_RELEASED, NULL);

    audio_icon_button(body, landscape ? group_x : control_x + 54,
                      secondary_y, 64, "i", 0x242C36,
                      music_info_open_cb, NULL);
    audio_icon_button(body, landscape ? group_x + 64 + group_gap :
                      control_x + 136, secondary_y, 64, LV_SYMBOL_LIST, 0x242C36,
                      music_list_open_cb, NULL);
    memset(music_mode_btn, 0, sizeof(music_mode_btn));
    music_create_mode_button(body, landscape ? group_x + (64 + group_gap) * 2 :
                             control_x + 218, secondary_y, LV_SYMBOL_LOOP, "1",
                             MUSIC_MODE_REPEAT_ONE);
    music_create_mode_button(body, landscape ? group_x + (64 + group_gap) * 3 :
                             control_x + 300, secondary_y, LV_SYMBOL_LOOP, NULL,
                             MUSIC_MODE_LIST_LOOP);
    music_create_mode_button(body, landscape ? group_x + (64 + group_gap) * 4 :
                             control_x + 382, secondary_y, LV_SYMBOL_SHUFFLE, NULL,
                             MUSIC_MODE_SHUFFLE);
    audio_create_volume_control(body, volume_x, volume_y, volume_w,
                                &music_volume_slider, &music_volume_label);

    memset(music_row, 0, sizeof(music_row));
    memset(music_row_icon, 0, sizeof(music_row_icon));
    memset(music_row_meta, 0, sizeof(music_row_meta));
    music_create_list_overlay(scr);
    music_create_info_overlay(scr);

    audio_ensure_timer();
    music_refresh_ui();
}

void ui_recorder_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *dial;
    lv_obj_t *dial_icon;
    lv_obj_t *hint;
    lv_obj_t *info;
    int landscape = ui_is_landscape();
    int screen_w = ui_screen_width();
    int body_h = ui_body_height(144);
    int left_region_w = landscape ? screen_w / 2 - 48 : 0;
    int dial_size = landscape ? body_h - 40 : 500;
    int dial_x;
    int control_x;
    int control_w;
    int file_y = landscape ? 26 : 552;
    int controls_y = landscape ? 118 : 654;
    int record_y = landscape ? 88 : 624;
    int info_y = landscape ? 260 : 802;
    int info_h = landscape ? body_h - info_y - 24 : 252;
    int center_x;

    if(landscape) {
        if(dial_size > left_region_w - 24) {
            dial_size = left_region_w - 24;
        }
        if(dial_size > 464) {
            dial_size = 464;
        }
        if(dial_size < 330) {
            dial_size = body_h > 360 ? 330 : body_h - 32;
        }
    } else {
        if(dial_size < 250) {
            dial_size = 250;
        }
    }
    dial_x = landscape ? 24 + (left_region_w - dial_size) / 2 : 34;
    if(dial_x < 24) {
        dial_x = 24;
    }
    control_x = landscape ? screen_w / 2 + 12 : 34;
    control_w = landscape ? screen_w - control_x - 24 : 500;
    if(control_w < 340) {
        control_w = 340;
    }
    if(info_h < 152) {
        info_h = 152;
    }
    center_x = control_x + control_w / 2;

    active_audio_page = PAGE_RECORDER;
    recorder_ensure_input_route("Recorder");
    audio_load_volume_once();
    recorder_scan_recordings();
    if(player_pid <= 0 && recorder_pid <= 0) {
        audio_set_status("Ready");
    }

    ui_create_header(scr, "Recorder");

    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    dial = ui_panel(body, dial_x, landscape ? 20 : 28, dial_size, dial_size);
    lv_obj_set_style_bg_color(dial, lv_color_hex(0x20151A), 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(0x4A2632), 0);
    lv_obj_set_style_pad_all(dial, 0, 0);

    dial_icon = ui_label(dial, AUDIO_ICON_MIC, &lv_font_montserrat_32,
                         0xEF4D5A);
    lv_obj_align(dial_icon, LV_ALIGN_CENTER, 0,
                 landscape ? -(dial_size / 4) : -112);

    recorder_timer_label = ui_label(dial, "00:00", &lv_font_montserrat_32,
                                    0xF2F5F8);
    lv_obj_align(recorder_timer_label, LV_ALIGN_CENTER, 0, -16);

    recorder_status_label = ui_label(dial, "Ready", &lv_font_montserrat_20,
                                     0xF5A524);
    lv_obj_set_width(recorder_status_label, dial_size - 60);
    lv_label_set_long_mode(recorder_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(recorder_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(recorder_status_label, LV_ALIGN_CENTER, 0,
                 landscape ? dial_size / 7 : 52);

    hint = ui_label(dial, "WAV 48kHz mono", &lv_font_montserrat_16,
                    0x9AA4AF);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -36);

    recorder_file_label = ui_label(body, "No recording selected",
                                   &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_set_width(recorder_file_label, control_w);
    lv_label_set_long_mode(recorder_file_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(recorder_file_label,
                                landscape ? LV_TEXT_ALIGN_LEFT :
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(recorder_file_label, control_x, file_y);

    recorder_play_label = audio_icon_button(body,
                                            landscape ? center_x - 172 : 112,
                                            controls_y, 82, LV_SYMBOL_PLAY,
                                            0x242C36, recorder_play_cb, NULL);
    recorder_record_label = audio_icon_button(body,
                                              landscape ? center_x - 62 : 222,
                                              record_y, 124,
                                              AUDIO_ICON_MIC, 0xEF4D5A,
                                              recorder_record_cb, NULL);
    audio_icon_button(body, landscape ? center_x + 114 : 374, controls_y, 82,
                      LV_SYMBOL_LIST, 0x242C36,
                      recorder_list_open_cb, NULL);

    info = ui_panel(body, landscape ? control_x : 24, info_y,
                    landscape ? control_w : 520, info_h);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x151B22), 0);
    if(landscape) {
        int info_w = control_w - 32;

        recorder_device_label = audio_create_stack_info_row(info, 10, "Device",
                                                            "--", 0xF2F5F8,
                                                            info_w);
        audio_create_stack_info_row(info, 76, "Folder", AUDIO_RECORD_DIR,
                                    0x9AA4AF, info_w);
        recorder_count_label = audio_create_stack_info_row(info, 142, "Files",
                                                           "0", 0xEF4D5A,
                                                           info_w);
    } else {
        ui_info_row(info, 18, "Device", "--", 0xF2F5F8);
        recorder_device_label =
            lv_obj_get_child(info, lv_obj_get_child_count(info) - 1);
        ui_info_row(info, 82, "Folder", AUDIO_RECORD_DIR, 0x9AA4AF);
        ui_info_row(info, 146, "Files", "0", 0xEF4D5A);
        recorder_count_label =
            lv_obj_get_child(info, lv_obj_get_child_count(info) - 1);
    }

    memset(recorder_row, 0, sizeof(recorder_row));
    memset(recorder_row_icon, 0, sizeof(recorder_row_icon));
    memset(recorder_row_meta, 0, sizeof(recorder_row_meta));
    recorder_create_list_overlay(scr);

    audio_ensure_timer();
    recorder_refresh_ui();
}

void ui_net_radio_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *status;
    lv_obj_t *actions;
    lv_obj_t *list;
    lv_obj_t *btn;
    int landscape = ui_is_landscape();
    int body_h = ui_body_height(144);
    int left_w = landscape ? 420 : 520;
    int right_x = landscape ? 468 : 24;
    int right_w = landscape ? ui_screen_width() - right_x - 24 : 520;
    int status_h = landscape ? body_h - 48 : 286;
    int list_h = landscape ? body_h - 142 : 608;
    int button_w;
    int volume_y = landscape ? status_h - 64 : status_h - 54;
    int volume_w = landscape ? left_w - 72 : left_w - 32;

    if(right_w < 340) {
        right_w = 340;
    }
    if(status_h < 260) {
        status_h = 260;
    }
    if(list_h < 180) {
        list_h = 180;
    }
    button_w = (right_w - 44) / 2;

    active_audio_page = PAGE_NET_RADIO;
    audio_load_streams();
    audio_load_volume_once();
    if(player_pid <= 0) {
        snprintf(player_title, sizeof(player_title), "%s", "Radio");
        snprintf(player_source, sizeof(player_source), "%s", "HTTP MP3");
        audio_set_status("Ready");
    }

    ui_create_header(scr, "Radio");
    body = ui_page_body(scr, 144);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101418), 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    status = ui_panel(body, 24, 24, left_w, status_h);
    lv_obj_set_style_bg_color(status, lv_color_hex(0x11231A), 0);
    if(landscape) {
        int info_w = left_w - 32;

        radio_state_label = audio_create_stack_info_row(status, 14, "State",
                                                        player_status, 0xF5A524,
                                                        info_w);
        radio_network_label = audio_create_stack_info_row(status, 78, "Network",
                                                          "--", 0x25C281,
                                                          info_w);
        radio_device_label = audio_create_stack_info_row(status, 142, "Device",
                                                         "--", 0xF2F5F8,
                                                         info_w);
        radio_count_label = audio_create_stack_info_row(status, 218, "Streams",
                                                        "0", 0x9AA4AF,
                                                        info_w);
    } else {
        ui_info_row(status, 16, "State", player_status, 0xF5A524);
        radio_state_label =
            lv_obj_get_child(status, lv_obj_get_child_count(status) - 1);
        ui_info_row(status, 70, "Network", "--", 0x25C281);
        radio_network_label =
            lv_obj_get_child(status, lv_obj_get_child_count(status) - 1);
        ui_info_row(status, 124, "Device", "--", 0xF2F5F8);
        radio_device_label =
            lv_obj_get_child(status, lv_obj_get_child_count(status) - 1);
        ui_info_row(status, 178, "Streams", "0", 0x9AA4AF);
        radio_count_label =
            lv_obj_get_child(status, lv_obj_get_child_count(status) - 1);
    }
    audio_create_volume_control(status, 0, volume_y, volume_w,
                                &radio_volume_slider, &radio_volume_label);

    actions = ui_panel(body, right_x, landscape ? 24 : 332,
                       right_w, 92);
    lv_obj_set_style_bg_color(actions, lv_color_hex(0x151B22), 0);
    btn = ui_command_button(actions, 0, 14, button_w, "Custom URL", 0x25C281);
    lv_obj_add_event_cb(btn, radio_custom_cb, LV_EVENT_CLICKED, NULL);
    btn = ui_command_button(actions, button_w + 20, 14, button_w, "Stop", 0xF5A524);
    lv_obj_add_event_cb(btn, radio_stop_cb, LV_EVENT_CLICKED, NULL);

    list = ui_panel(body, right_x, landscape ? 118 : 446,
                    right_w, list_h);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(list, 16, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    memset(radio_row, 0, sizeof(radio_row));
    memset(radio_row_icon, 0, sizeof(radio_row_icon));
    for(int i = 0; i < stream_count; i++) {
        radio_row[i] = audio_create_row(list, i * 82, LV_SYMBOL_PLAY,
                                        streams[i].name, streams[i].url,
                                        0x25C281, radio_row_cb,
                                        (void *)(intptr_t)i,
                                        0,
                                        &radio_row_icon[i], NULL);
    }

    audio_ensure_timer();
    radio_refresh_ui();
}

void ui_audio_cleanup(void)
{
    recorder_close_delete_confirm();
    audio_notification_reap();

    if(recorder_input_route_active) {
        if(recorder_pid > 0) {
            recorder_stop();
        }
        ui_audio_input_route_leave("Recorder");
        recorder_input_route_active = 0;
        recorder_playback_route_suspended = 0;
        audio_debug_log("REC_INPUT_ROUTE_LEAVE restored=%s",
                        ui_audio_output_is_external() ? "external" : "headphones");
    } else if(recorder_playback_route_suspended) {
        recorder_playback_route_suspended = 0;
        audio_debug_log("REC_INPUT_ROUTE_SUSPEND_CLEAR output=%s",
                        ui_audio_output_is_external() ? "external" : "headphones");
    }

    if(audio_timer && player_pid <= 0 && recorder_pid <= 0) {
        lv_timer_delete(audio_timer);
        audio_timer = NULL;
    }

    ui_input_dialog_close_active();
    active_audio_page = PAGE_HOME;

    music_title_label = NULL;
    music_meta_label = NULL;
    music_state_label = NULL;
    music_device_label = NULL;
    music_count_label = NULL;
    music_play_label = NULL;
    music_progress_slider = NULL;
    music_progress_label = NULL;
    music_cover_image = NULL;
    music_cover_placeholder = NULL;
    cover_render_index = -2;
    music_volume_slider = NULL;
    music_volume_label = NULL;
    music_list_overlay = NULL;
    music_info_overlay = NULL;
    music_info_title_label = NULL;
    music_info_path_label = NULL;
    music_info_duration_label = NULL;
    music_progress_dragging = 0;
    radio_state_label = NULL;
    radio_network_label = NULL;
    radio_device_label = NULL;
    radio_count_label = NULL;
    radio_volume_slider = NULL;
    radio_volume_label = NULL;
    recorder_timer_label = NULL;
    recorder_status_label = NULL;
    recorder_file_label = NULL;
    recorder_device_label = NULL;
    recorder_count_label = NULL;
    recorder_record_label = NULL;
    recorder_play_label = NULL;
    recorder_list_overlay = NULL;
    recorder_list_back = NULL;
    recorder_list_title = NULL;
    recorder_list_panel = NULL;
    notification_status_label = NULL;
    notification_current_label = NULL;
    memset(music_mode_btn, 0, sizeof(music_mode_btn));
    memset(music_row, 0, sizeof(music_row));
    memset(music_row_icon, 0, sizeof(music_row_icon));
    memset(music_row_meta, 0, sizeof(music_row_meta));
    memset(radio_row, 0, sizeof(radio_row));
    memset(radio_row_icon, 0, sizeof(radio_row_icon));
    memset(recorder_row, 0, sizeof(recorder_row));
    memset(recorder_row_icon, 0, sizeof(recorder_row_icon));
    memset(recorder_row_meta, 0, sizeof(recorder_row_meta));
    memset(notification_row, 0, sizeof(notification_row));
    memset(notification_row_meta, 0, sizeof(notification_row_meta));
}
