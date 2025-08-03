#include <cjson/cJSON.h>
#include <dirent.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <raylib.h>
#include <rlgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define WIDTH 1080
#define HEIGHT 1920
#define FPS 60
#define SLIDE_SPEED 20.0f
#define CHARACTER_SCALE 0.5f
#define MAX_CAPTIONS 1000
#define MAX_TEXT_LENGTH 512

// Timing utilities for performance debugging
static double get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

#define TIMING_START(name) \
    double timing_start_##name = get_time_ms()

#define TIMING_END(name) \
    double timing_end_##name = get_time_ms(); \
    printf("[TIMING] %s: %.2fms\n", #name, timing_end_##name - timing_start_##name)

typedef enum { PETER, STEWIE } Character;

typedef struct {
  float x;
  float targetX;
  float startX;
  float alpha;
  float slideProgress;
  bool isVisible;
  bool isSliding;
  bool isFading;
} CharacterState;

typedef struct {
  float startTime;
  float endTime;
  char text[MAX_TEXT_LENGTH];
  Character speaker;
  // Word timing data
  struct {
    char word[64];
    float start;
    float end;
  } words[100];
  int wordCount;
} Caption;

// JSON parser for caption files using cJSON
int loadCaptions(const char *projectId, Caption *captions, int maxCaptions) {
  char dirPath[256];
  snprintf(dirPath, sizeof(dirPath), "media/captions/%s", projectId);

  DIR *dir = opendir(dirPath);
  if (!dir) {
    printf("Warning: Could not open captions directory: %s\n", dirPath);
    return 0;
  }

  // Get all JSON files and sort them by filename
  struct dirent *entries[1000];
  int fileCount = 0;
  struct dirent *entry;

  while ((entry = readdir(dir)) != NULL && fileCount < 1000) {
    if (strstr(entry->d_name, ".json") != NULL) {
      entries[fileCount] = malloc(sizeof(struct dirent));
      *entries[fileCount] = *entry;
      fileCount++;
    }
  }
  closedir(dir);

  // Simple sort by filename
  for (int i = 0; i < fileCount - 1; i++) {
    for (int j = i + 1; j < fileCount; j++) {
      if (strcmp(entries[i]->d_name, entries[j]->d_name) > 0) {
        struct dirent *temp = entries[i];
        entries[i] = entries[j];
        entries[j] = temp;
      }
    }
  }

  int captionCount = 0;
  float currentTimeOffset = 0.0f;

  for (int fileIdx = 0; fileIdx < fileCount && captionCount < maxCaptions;
       fileIdx++) {
    char filePath[512];
    snprintf(filePath, sizeof(filePath), "%s/%s", dirPath,
             entries[fileIdx]->d_name);

    FILE *file = fopen(filePath, "r");
    if (!file) {
      free(entries[fileIdx]);
      continue;
    }

    // Read entire file
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *jsonContent = malloc(fileSize + 1);
    fread(jsonContent, 1, fileSize, file);
    jsonContent[fileSize] = '\0';
    fclose(file);

    // Parse JSON
    cJSON *json = cJSON_Parse(jsonContent);
    if (!json) {
      printf("Error parsing JSON in file: %s\n", entries[fileIdx]->d_name);
      free(jsonContent);
      free(entries[fileIdx]);
      continue;
    }

    // Extract transcript
    cJSON *transcript = cJSON_GetObjectItem(json, "transcript");
    if (cJSON_IsString(transcript)) {
      strncpy(captions[captionCount].text, transcript->valuestring,
              MAX_TEXT_LENGTH - 1);
      captions[captionCount].text[MAX_TEXT_LENGTH - 1] = '\0';

      // Determine speaker from filename
      if (strstr(entries[fileIdx]->d_name, "peter")) {
        captions[captionCount].speaker = PETER;
      } else if (strstr(entries[fileIdx]->d_name, "stewie")) {
        captions[captionCount].speaker = STEWIE;
      }

      // Parse word timing data
      captions[captionCount].wordCount = 0;
      cJSON *words = cJSON_GetObjectItem(json, "words");
      if (cJSON_IsArray(words)) {
        int wordIdx = 0;
        cJSON *word = NULL;

        cJSON_ArrayForEach(word, words) {
          if (wordIdx >= 100)
            break;

          cJSON *wordText = cJSON_GetObjectItem(word, "word");
          cJSON *startTime = cJSON_GetObjectItem(word, "start");
          cJSON *endTime = cJSON_GetObjectItem(word, "end");

          if (cJSON_IsString(wordText) && cJSON_IsNumber(startTime) &&
              cJSON_IsNumber(endTime)) {
            strncpy(captions[captionCount].words[wordIdx].word,
                    wordText->valuestring, 63);
            captions[captionCount].words[wordIdx].word[63] = '\0';

            // Add time offset to sequence the conversations
            captions[captionCount].words[wordIdx].start =
                startTime->valuedouble + currentTimeOffset;
            captions[captionCount].words[wordIdx].end =
                endTime->valuedouble + currentTimeOffset;
            wordIdx++;
          }
        }
        captions[captionCount].wordCount = wordIdx;
      }

      // Set overall timing based on first and last word
      if (captions[captionCount].wordCount > 0) {
        captions[captionCount].startTime =
            captions[captionCount].words[0].start;
        captions[captionCount].endTime =
            captions[captionCount]
                .words[captions[captionCount].wordCount - 1]
                .end;

        // Update time offset for next file (add 0.5 second gap)
        currentTimeOffset = captions[captionCount].endTime + 0.5f;
      } else {
        // Fallback timing
        captions[captionCount].startTime = currentTimeOffset;
        captions[captionCount].endTime = currentTimeOffset + 3.0f;
        currentTimeOffset += 3.5f;
      }

      captionCount++;
    }

    cJSON_Delete(json);
    free(jsonContent);
    free(entries[fileIdx]);
  }

  printf("Loaded %d captions from %s (total duration: %.1fs)\n", captionCount,
         dirPath, currentTimeOffset);

  return captionCount;
}

// Background video decoder context for on-demand loading
typedef struct {
  AVFormatContext *fmt_ctx;
  AVCodecContext *codec_ctx;
  AVStream *video_stream;
  struct SwsContext *sws_ctx;
  AVFrame *frame;
  AVFrame *sw_frame;  // Software frame for CPU access
  AVPacket *pkt;
  int stream_index;
  double time_base;
  int64_t start_time;
} BackgroundVideo;

// Audio mixer context
typedef struct {
  AVFormatContext *fmt_ctx;
  AVCodecContext *codec_ctx;
  AVStream *audio_stream;
  struct SwrContext *swr_ctx;
  AVFrame *frame;
  AVPacket *pkt;
  int stream_index;
  double time_base;
  uint8_t **audio_data;
  int audio_linesize;
  int sample_rate;
  int channels;
  int64_t pts;
  // Preloaded audio buffer
  float *stereo_buffer; // 44.1kHz stereo float samples
  int buffer_samples;   // Total samples in buffer
} AudioFile;

// Forward declarations
int getBackgroundFrame(BackgroundVideo *bg, double target_time, uint8_t *rgba_buffer);

// Initialize background video decoder
int initBackgroundVideo(BackgroundVideo *bg, const char *filename) {
  memset(bg, 0, sizeof(BackgroundVideo));
  bg->stream_index = -1;

  // Open video file
  if (avformat_open_input(&bg->fmt_ctx, filename, NULL, NULL) < 0) {
    printf("Error: Could not open background video file: %s\n", filename);
    return -1;
  }

  if (avformat_find_stream_info(bg->fmt_ctx, NULL) < 0) {
    printf("Error: Could not find stream information\n");
    return -1;
  }

  // Find video stream
  for (unsigned int i = 0; i < bg->fmt_ctx->nb_streams; i++) {
    if (bg->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      bg->stream_index = i;
      bg->video_stream = bg->fmt_ctx->streams[i];
      break;
    }
  }

  if (bg->stream_index == -1) {
    printf("Error: No video stream found\n");
    return -1;
  }

  // Find H.264 decoder with hardware acceleration support
  const AVCodec *codec = avcodec_find_decoder(bg->video_stream->codecpar->codec_id);
  if (!codec) {
    printf("Error: Could not find decoder for codec\n");
    return -1;
  }
  
  printf("Found decoder: %s\n", codec->name);

  // Allocate codec context
  bg->codec_ctx = avcodec_alloc_context3(codec);
  if (!bg->codec_ctx) {
    printf("Error: Could not allocate codec context\n");
    return -1;
  }

  // Copy codec parameters
  if (avcodec_parameters_to_context(bg->codec_ctx, bg->video_stream->codecpar) < 0) {
    printf("Error: Could not copy codec parameters\n");
    return -1;
  }

  // Try VAAPI hardware acceleration first
  AVDictionary *opts = NULL;
  av_dict_set(&opts, "hwaccel", "vaapi", 0);
  av_dict_set(&opts, "hwaccel_device", "/dev/dri/renderD128", 0);

  if (avcodec_open2(bg->codec_ctx, codec, &opts) >= 0) {
    printf("Successfully initialized VAAPI hardware acceleration\n");
  } else {
    printf("Warning: VAAPI failed, trying software decoder\n");
    av_dict_free(&opts);
    avcodec_free_context(&bg->codec_ctx);

    // Fallback to software decoder
    bg->codec_ctx = avcodec_alloc_context3(codec);
    if (!bg->codec_ctx || 
        avcodec_parameters_to_context(bg->codec_ctx, bg->video_stream->codecpar) < 0 ||
        avcodec_open2(bg->codec_ctx, codec, NULL) < 0) {
      printf("Error: Could not initialize decoder\n");
      return -1;
    }
    printf("Using software decoder\n");
  }
  av_dict_free(&opts);

  // Allocate frames and packet
  bg->frame = av_frame_alloc();
  bg->sw_frame = av_frame_alloc();
  bg->pkt = av_packet_alloc();

  if (!bg->frame || !bg->sw_frame || !bg->pkt) {
    printf("Error: Could not allocate frames or packet\n");
    return -1;
  }

  // Verify video dimensions (should be pre-scaled to 1080x1920)
  int src_width = bg->codec_ctx->width;
  int src_height = bg->codec_ctx->height;
  
  if (src_width != WIDTH || src_height != HEIGHT) {
    printf("Warning: Video dimensions %dx%d don't match expected %dx%d\n", 
           src_width, src_height, WIDTH, HEIGHT);
    printf("Video should be pre-scaled to 1080x1920 for optimal performance\n");
  }

  // Simple YUV to RGBA conversion context (no scaling)
  bg->sws_ctx = sws_getContext(src_width, src_height, bg->codec_ctx->pix_fmt,
                               WIDTH, HEIGHT, AV_PIX_FMT_RGBA,
                               SWS_FAST_BILINEAR, NULL, NULL, NULL);

  if (!bg->sws_ctx) {
    printf("Error: Could not initialize color conversion context\n");
    return -1;
  }

  bg->time_base = av_q2d(bg->video_stream->time_base);
  bg->start_time = bg->video_stream->start_time;

  printf("Background video initialized: %dx%d, time_base: %f\n",
         bg->codec_ctx->width, bg->codec_ctx->height, bg->time_base);

  return 0;
}




// Get background video frame at specific time on-demand
int getBackgroundFrame(BackgroundVideo *bg, double target_time, uint8_t *rgba_buffer) {
  // Clear the buffer to black first
  memset(rgba_buffer, 0, WIDTH * HEIGHT * 4);

  // Calculate target PTS for seeking
  int64_t target_pts = (int64_t)(target_time / bg->time_base);
  if (bg->start_time != AV_NOPTS_VALUE) {
    target_pts += bg->start_time;
  }

  // Smart seeking - only seek for large jumps or backwards
  static double last_target_time = -1.0;
  static bool first_seek = true;

  bool should_seek = first_seek || 
                     target_time < last_target_time || 
                     (target_time - last_target_time) > 0.5;

  if (should_seek) {
    int seek_flags = AVSEEK_FLAG_BACKWARD;
    if (av_seek_frame(bg->fmt_ctx, bg->stream_index, target_pts, seek_flags) >= 0) {
      avcodec_flush_buffers(bg->codec_ctx);
    }
    first_seek = false;
  }
  last_target_time = target_time;

  // Decode frames until we find the target
  while (av_read_frame(bg->fmt_ctx, bg->pkt) >= 0) {
    if (bg->pkt->stream_index == bg->stream_index) {
      if (avcodec_send_packet(bg->codec_ctx, bg->pkt) >= 0) {
        while (avcodec_receive_frame(bg->codec_ctx, bg->frame) >= 0) {
          int64_t frame_pts = bg->frame->pts;
          if (bg->start_time != AV_NOPTS_VALUE) {
            frame_pts -= bg->start_time;
          }

          double frame_time = frame_pts * bg->time_base;

          // Use this frame if it's at or past our target time
          if (frame_time >= target_time - 0.016) { // Within one frame at 60fps
            // Check if this is a hardware frame that needs transfer
            AVFrame *src_frame = bg->frame;
            if (bg->frame->format == AV_PIX_FMT_VAAPI) {
              int ret = av_hwframe_transfer_data(bg->sw_frame, bg->frame, 0);
              if (ret < 0) {
                printf("Error: Failed to transfer frame from GPU to CPU (ret=%d)\n", ret);
                av_packet_unref(bg->pkt);
                return -1;
              }
              src_frame = bg->sw_frame;
            }

            // Direct conversion from YUV to RGBA (video is pre-scaled to 1080x1920)
            const uint8_t *src_data[4] = {src_frame->data[0], src_frame->data[1], src_frame->data[2], NULL};
            int src_linesize[4] = {src_frame->linesize[0], src_frame->linesize[1], src_frame->linesize[2], 0};
            uint8_t *dst_data[1] = {rgba_buffer};
            int dst_linesize[1] = {WIDTH * 4};

            sws_scale(bg->sws_ctx, src_data, src_linesize, 0, HEIGHT, dst_data, dst_linesize);

            av_packet_unref(bg->pkt);
            return 0;
          }
        }
      }
    }
    av_packet_unref(bg->pkt);
  }

  return -1; // No frame found
}

// Cleanup background video
void cleanupBackgroundVideo(BackgroundVideo *bg) {
  if (bg->sws_ctx)
    sws_freeContext(bg->sws_ctx);
  if (bg->frame)
    av_frame_free(&bg->frame);
  if (bg->sw_frame)
    av_frame_free(&bg->sw_frame);
  if (bg->pkt)
    av_packet_free(&bg->pkt);
  if (bg->codec_ctx)
    avcodec_free_context(&bg->codec_ctx);
  if (bg->fmt_ctx)
    avformat_close_input(&bg->fmt_ctx);
}

// Load audio files for mixing
int loadAudioFiles(const char *projectId, AudioFile **audioFiles,
                   int *audioCount) {
  char audioDir[256];
  snprintf(audioDir, sizeof(audioDir), "media/audio/%s", projectId);

  DIR *dir = opendir(audioDir);
  if (!dir) {
    printf("Warning: Could not open audio directory: %s\n", audioDir);
    return 0;
  }

  // Get all WAV files and sort them by filename (same as captions)
  struct dirent *entries[1000];
  int fileCount = 0;
  struct dirent *entry;

  while ((entry = readdir(dir)) != NULL && fileCount < 1000) {
    if (strstr(entry->d_name, ".wav") != NULL) {
      entries[fileCount] = malloc(sizeof(struct dirent));
      *entries[fileCount] = *entry;
      fileCount++;
    }
  }
  closedir(dir);

  // Simple sort by filename (matches caption loading logic)
  for (int i = 0; i < fileCount - 1; i++) {
    for (int j = i + 1; j < fileCount; j++) {
      if (strcmp(entries[i]->d_name, entries[j]->d_name) > 0) {
        struct dirent *temp = entries[i];
        entries[i] = entries[j];
        entries[j] = temp;
      }
    }
  }

  if (fileCount == 0) {
    printf("No audio files found in %s\n", audioDir);
    return 0;
  }

  *audioFiles = malloc(fileCount * sizeof(AudioFile));
  *audioCount = 0;

  // Load each WAV file in sorted order
  for (int fileIdx = 0; fileIdx < fileCount; fileIdx++) {
    char filePath[512];
    snprintf(filePath, sizeof(filePath), "%s/%s", audioDir,
             entries[fileIdx]->d_name);

    AudioFile *af = &(*audioFiles)[*audioCount];
    memset(af, 0, sizeof(AudioFile));
    af->stream_index = -1;

    // Open audio file
    if (avformat_open_input(&af->fmt_ctx, filePath, NULL, NULL) >= 0) {
      if (avformat_find_stream_info(af->fmt_ctx, NULL) >= 0) {
        // Find audio stream
        for (unsigned int i = 0; i < af->fmt_ctx->nb_streams; i++) {
          if (af->fmt_ctx->streams[i]->codecpar->codec_type ==
              AVMEDIA_TYPE_AUDIO) {
            af->stream_index = i;
            af->audio_stream = af->fmt_ctx->streams[i];
            break;
          }
        }

        if (af->stream_index != -1) {
          // Initialize decoder
          const AVCodec *codec =
              avcodec_find_decoder(af->audio_stream->codecpar->codec_id);
          if (codec) {
            af->codec_ctx = avcodec_alloc_context3(codec);
            if (avcodec_parameters_to_context(
                    af->codec_ctx, af->audio_stream->codecpar) >= 0) {
              if (avcodec_open2(af->codec_ctx, codec, NULL) >= 0) {
                af->time_base = av_q2d(af->audio_stream->time_base);
                af->sample_rate = af->codec_ctx->sample_rate;
                af->channels = af->codec_ctx->ch_layout.nb_channels;
                af->frame = av_frame_alloc();
                af->pkt = av_packet_alloc();

                // Initialize resampler for format conversion with high quality
                af->swr_ctx = swr_alloc();
                if (af->swr_ctx) {
                  av_opt_set_chlayout(af->swr_ctx, "in_chlayout",
                                      &af->codec_ctx->ch_layout, 0);
                  av_opt_set_int(af->swr_ctx, "in_sample_rate", af->sample_rate,
                                 0);
                  av_opt_set_sample_fmt(af->swr_ctx, "in_sample_fmt",
                                        af->codec_ctx->sample_fmt, 0);

                  AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
                  av_opt_set_chlayout(af->swr_ctx, "out_chlayout",
                                      &stereo_layout, 0);
                  av_opt_set_int(af->swr_ctx, "out_sample_rate", 44100, 0);
                  av_opt_set_sample_fmt(af->swr_ctx, "out_sample_fmt",
                                        AV_SAMPLE_FMT_FLTP, 0);

                  // Use default resampling settings for stability

                  if (swr_init(af->swr_ctx) < 0) {
                    swr_free(&af->swr_ctx);
                    af->swr_ctx = NULL;
                  } else {
                    // Preload entire audio file into memory
                    printf("Preloading audio file: %s\n",
                           entries[fileIdx]->d_name);

                    // Calculate actual file duration and buffer size
                    double duration = 0.0;
                    if (af->fmt_ctx->duration != AV_NOPTS_VALUE) {
                      duration = (double)af->fmt_ctx->duration / AV_TIME_BASE;
                    } else if (af->audio_stream->duration != AV_NOPTS_VALUE) {
                      duration = af->audio_stream->duration * av_q2d(af->audio_stream->time_base);
                    } else {
                      // Fallback: estimate from file size (rough approximation)
                      duration = 10.0; // Default to 10 seconds if duration unknown
                    }
                    
                    printf("Audio file duration: %.2f seconds\n", duration);
                    
                    // Allocate buffer based on actual duration + 10% safety margin
                    int estimated_samples = (int)(44100 * duration * 1.1);
                    af->stereo_buffer = malloc(estimated_samples * 2 * sizeof(float));
                    af->buffer_samples = 0;

                    if (af->stereo_buffer) {
                      // Read entire file and convert to 44.1kHz stereo
                      while (av_read_frame(af->fmt_ctx, af->pkt) >= 0) {
                        if (af->pkt->stream_index == af->stream_index) {
                          if (avcodec_send_packet(af->codec_ctx, af->pkt) >=
                              0) {
                            while (avcodec_receive_frame(af->codec_ctx,
                                                         af->frame) >= 0) {
                              uint8_t **out_data = NULL;
                              int out_linesize;
                              int out_samples =
                                  av_rescale_rnd(af->frame->nb_samples, 44100,
                                                 af->sample_rate, AV_ROUND_UP);

                              if (av_samples_alloc_array_and_samples(
                                      &out_data, &out_linesize, 2, out_samples,
                                      AV_SAMPLE_FMT_FLTP, 0) >= 0) {
                                int converted = swr_convert(
                                    af->swr_ctx, out_data, out_samples,
                                    (const uint8_t **)af->frame->data,
                                    af->frame->nb_samples);

                                if (converted > 0) {
                                  // Check if we need to resize buffer
                                  if (af->buffer_samples + converted >= estimated_samples) {
                                    printf("Warning: Audio file longer than estimated, expanding buffer\n");
                                    estimated_samples = (af->buffer_samples + converted) * 2; // Double the size
                                    af->stereo_buffer = realloc(af->stereo_buffer, estimated_samples * 2 * sizeof(float));
                                    if (!af->stereo_buffer) {
                                      printf("Error: Could not expand audio buffer\n");
                                      break;
                                    }
                                  }
                                  
                                  float *left = (float *)out_data[0];
                                  float *right = (float *)out_data[1];

                                  // Simple interleave stereo samples
                                  for (int i = 0; i < converted; i++) {
                                    af->stereo_buffer[(af->buffer_samples + i) * 2] = left[i];
                                    af->stereo_buffer[(af->buffer_samples + i) * 2 + 1] = right[i];
                                  }
                                  af->buffer_samples += converted;
                                }

                                av_freep(&out_data[0]);
                                av_freep(&out_data);
                              }
                            }
                          }
                        }
                        av_packet_unref(af->pkt);
                      }

                      printf("Preloaded %d samples (%.2f seconds)\n",
                             af->buffer_samples,
                             (float)af->buffer_samples / 44100.0f);
                    }

                    // Reset to beginning for potential future use
                    av_seek_frame(af->fmt_ctx, af->stream_index, 0,
                                  AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(af->codec_ctx);
                  }
                }

                printf("Loaded audio file: %s (SR: %d, Ch: %d)\n",
                       entries[fileIdx]->d_name, af->sample_rate, af->channels);
                (*audioCount)++;
              } else {
                avcodec_free_context(&af->codec_ctx);
                avformat_close_input(&af->fmt_ctx);
              }
            } else {
              avcodec_free_context(&af->codec_ctx);
              avformat_close_input(&af->fmt_ctx);
            }
          } else {
            avformat_close_input(&af->fmt_ctx);
          }
        } else {
          avformat_close_input(&af->fmt_ctx);
        }
      } else {
        avformat_close_input(&af->fmt_ctx);
      }
    }

    free(entries[fileIdx]);
  }

  printf("Loaded %d audio files from %s\n", *audioCount, audioDir);
  return *audioCount;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: %s <projectId> [--render <background_video>]\n", argv[0]);
    printf("  Normal mode: %s projectId\n", argv[0]);
    printf("  Render mode: %s projectId --render ./media/parkour1.mp4\n",
           argv[0]);
    printf("  Audio files will be loaded from ./media/audio/projectId/\n");
    return 1;
  }

  const char *projectId = argv[1];
  bool renderMode = false;
  const char *backgroundVideo = NULL;

  // Parse arguments
  if (argc >= 4 && strcmp(argv[2], "--render") == 0) {
    renderMode = true;
    backgroundVideo = argv[3];
    printf("Render mode: background=%s, audio from ./media/audio/%s/\n",
           backgroundVideo, projectId);
  }
  InitWindow(WIDTH, HEIGHT, "Peter & Stewie TikTok Format");

  if (renderMode) {
    // Hide window and disable VSync for maximum render speed
    SetWindowState(FLAG_WINDOW_HIDDEN);
    SetTargetFPS(0); // Unlimited FPS for fastest rendering
    // Disable all window decorations and minimize overhead
    SetConfigFlags(FLAG_WINDOW_UNDECORATED);
    printf("Render mode: Running headless for maximum speed\n");
  } else {
    SetTargetFPS(FPS);
  }

  // Load font at high resolution
  Font boldFont =
      LoadFontEx("./media/theboldfont.ttf", 128, NULL, 0); // Load at 128px
  if (boldFont.texture.id == 0) {
    printf("Warning: Could not load theboldfont.ttf, using default font\n");
    boldFont = GetFontDefault();
  } else {
    // Set texture filter to bilinear for better scaling quality
    SetTextureFilter(boldFont.texture, TEXTURE_FILTER_BILINEAR);
  }

  // Load captions
  Caption captions[MAX_CAPTIONS];
  int captionCount = loadCaptions(projectId, captions, MAX_CAPTIONS);

  // Calculate total duration from captions
  float totalDuration = 10.0f; // Default fallback
  if (captionCount > 0) {
    // Find the actual end time of the last caption
    float maxEndTime = 0.0f;
    for (int i = 0; i < captionCount; i++) {
      if (captions[i].endTime > maxEndTime) {
        maxEndTime = captions[i].endTime;
      }
    }
    totalDuration = maxEndTime + 1.0f; // Add 1 second buffer
  }

  int FRAME_COUNT = (int)(FPS * totalDuration);
  printf("Video duration: %.1f seconds (%d frames)\n", totalDuration,
         FRAME_COUNT);

  // Load character textures
  Texture2D peterTexture = LoadTexture("./peter.png");
  Texture2D stewieTexture = LoadTexture("./stewie.png");

  // Set texture filter to bilinear for better scaling quality
  if (peterTexture.id != 0) {
    SetTextureFilter(peterTexture, TEXTURE_FILTER_BILINEAR);
  } else {
    printf("Warning: Could not load peter.png\n");
  }

  if (stewieTexture.id != 0) {
    SetTextureFilter(stewieTexture, TEXTURE_FILTER_BILINEAR);
  } else {
    printf("Warning: Could not load stewie.png\n");
  }

  // Calculate scaled dimensions
  int peterWidth =
      peterTexture.id != 0 ? (int)(peterTexture.width * CHARACTER_SCALE) : 400;
  int peterHeight =
      peterTexture.id != 0 ? (int)(peterTexture.height * CHARACTER_SCALE) : 600;
  int stewieWidth = stewieTexture.id != 0
                        ? (int)(stewieTexture.width * CHARACTER_SCALE)
                        : 400;
  int stewieHeight = stewieTexture.id != 0
                         ? (int)(stewieTexture.height * CHARACTER_SCALE)
                         : 600;

  // Character states - positioned at bottom, Peter stays left, Stewie stays
  // right
  CharacterState peter = {-peterWidth, 50,    -peterWidth, 0.0f,
                          0.0f,        false, false,       false};
  CharacterState stewie = {
      WIDTH, WIDTH - stewieWidth - 50, WIDTH, 0.0f, 0.0f, false, false, false};

  Character currentSpeaker = PETER;
  float speakerTimer = 0.0f;
  float currentTime = 0.0f;

  // FFmpeg setup for output
  AVFormatContext *fmt_ctx = NULL;
  AVCodecContext *video_codec_ctx = NULL;
  AVCodecContext *audio_codec_ctx = NULL;
  AVStream *video_st = NULL;
  AVStream *audio_st = NULL;
  AVFrame *video_frame = NULL;
  AVFrame *audio_frame = NULL;
  struct SwsContext *sws_ctx = NULL;

  // Background video and audio setup
  BackgroundVideo bgVideo = {0};
  AudioFile *audioFiles = NULL;
  int audioFileCount = 0;
  uint8_t *backgroundBuffer = NULL;
  Texture2D bgTexture = {0}; // Reusable background texture

  if (renderMode) {
    // Initialize background video
    if (initBackgroundVideo(&bgVideo, backgroundVideo) < 0) {
      printf("Error: Failed to initialize background video\n");
      return 1;
    }

    // Load audio files
    loadAudioFiles(projectId, &audioFiles, &audioFileCount);

    // Allocate background buffer
    backgroundBuffer = malloc(WIDTH * HEIGHT * 4); // RGBA
    if (!backgroundBuffer) {
      printf("Error: Could not allocate background buffer\n");
      return 1;
    }

    printf("Background video initialized for render mode\n");
  }

  // Setup output format with both video and audio
  const char *outputFile = renderMode ? "output_render.mp4" : "output.mp4";
  avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, outputFile);
  if (!fmt_ctx) {
    fprintf(stderr, "Could not create output context\n");
    return 1;
  }

  // Setup video codec with optimizations
  const AVCodec *video_codec = avcodec_find_encoder_by_name("h264_amf");
  if (!video_codec) {
    fprintf(stderr, "h264_amf encoder not found, falling back to libx264\n");
    video_codec = avcodec_find_encoder_by_name("libx264");
    if (!video_codec) {
      fprintf(stderr, "libx264 encoder not found\n");
      return 1;
    }
  }

  video_st = avformat_new_stream(fmt_ctx, video_codec);
  video_st->time_base = (AVRational){1, FPS};
  video_codec_ctx = avcodec_alloc_context3(video_codec);

  // Common settings
  video_codec_ctx->bit_rate = 8000000;
  video_codec_ctx->width = WIDTH;
  video_codec_ctx->height = HEIGHT;
  video_codec_ctx->time_base = video_st->time_base;
  video_codec_ctx->framerate = (AVRational){FPS, 1};
  video_codec_ctx->gop_size = 60;
  video_codec_ctx->max_b_frames = 0;
  video_codec_ctx->pix_fmt = (strstr(video_codec->name, "amf")) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;

  AVDictionary *encoder_opts = NULL;
  // AMF specific options
  if (strstr(video_codec->name, "amf")) {
    av_dict_set(&encoder_opts, "usage", "lowlatency", 0);
    av_dict_set(&encoder_opts, "profile", "main", 0);
    av_dict_set(&encoder_opts, "quality", "speed", 0);
    av_dict_set(&encoder_opts, "rc", "cqp", 0);
    av_dict_set(&encoder_opts, "qp_i", "23", 0);
    av_dict_set(&encoder_opts, "qp_p", "23", 0);
  } else {
    av_dict_set(&encoder_opts, "preset", "ultrafast", 0); // Fastest encoding
    av_dict_set(&encoder_opts, "tune", "zerolatency", 0); // Minimize latency
    av_dict_set(&encoder_opts, "crf", "28", 0); // Constant rate factor for speed
    av_dict_set(&encoder_opts, "threads", "0", 0); // Use all CPU threads
    av_dict_set(&encoder_opts, "thread_type", "slice+frame",
                0); // Enable both slice and frame threading
    av_dict_set(&encoder_opts, "x264-params",
                "aq-mode=0:me=dia:subme=1:ref=1:analyse=none:trellis=0:no-fast-"
                "pskip=0:8x8dct=0:sliced-threads=1",
                0);
  }

  if (avcodec_open2(video_codec_ctx, video_codec, &encoder_opts) < 0) {
    fprintf(stderr, "Could not open video codec\n");
    av_dict_free(&encoder_opts);
    return 1;
  }
  av_dict_free(&encoder_opts);

  printf("Successfully initialized %s encoder\n", video_codec->name);
  avcodec_parameters_from_context(video_st->codecpar, video_codec_ctx);

  // Setup audio codec if we have audio files
  if (renderMode && audioFileCount > 0) {
    const AVCodec *audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (audio_codec) {
      audio_st = avformat_new_stream(fmt_ctx, audio_codec);
      audio_codec_ctx = avcodec_alloc_context3(audio_codec);
      audio_codec_ctx->bit_rate = 128000; // Standard bitrate for stability
      audio_codec_ctx->sample_rate = 44100;
      audio_codec_ctx->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
      audio_codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
      audio_codec_ctx->time_base = (AVRational){1, 44100};

      if (avcodec_open2(audio_codec_ctx, audio_codec, NULL) >= 0) {
        avcodec_parameters_from_context(audio_st->codecpar, audio_codec_ctx);
        printf("Audio encoding enabled\n");
      } else {
        printf("Warning: Could not open audio codec\n");
        avcodec_free_context(&audio_codec_ctx);
        audio_codec_ctx = NULL;
      }
    }
  }

  if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&fmt_ctx->pb, outputFile, AVIO_FLAG_WRITE) < 0) {
      fprintf(stderr, "Could not open output file\n");
      return 1;
    }
  }
  if (avformat_write_header(fmt_ctx, NULL) < 0) {
    fprintf(stderr, "Error occurred when opening output file\n");
    return 1;
  }

  video_frame = av_frame_alloc();
  video_frame->format = video_codec_ctx->pix_fmt;
  video_frame->width = video_codec_ctx->width;
  video_frame->height = video_codec_ctx->height;
  av_frame_get_buffer(video_frame, 0);

  sws_ctx = sws_getContext(WIDTH, HEIGHT, AV_PIX_FMT_RGBA, WIDTH, HEIGHT,
                           video_codec_ctx->pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);

  static int64_t audio_sample_count = 0;
  // Pre-allocate audio buffers for performance
  const int audio_buffer_size = 8192; // Larger buffer to reduce allocations
  float *audio_buffer_left = malloc(audio_buffer_size * sizeof(float));
  float *audio_buffer_right = malloc(audio_buffer_size * sizeof(float));
  float *temp_left = malloc(audio_buffer_size * sizeof(float));
  float *temp_right = malloc(audio_buffer_size * sizeof(float));
  int audio_buffer_len = 0;
  int frame_idx = 0;
  
  // Pre-allocate RGBA buffer for direct rendering
  uint8_t *rgba_frame_buffer = malloc(WIDTH * HEIGHT * 4);
  if (!rgba_frame_buffer) {
    printf("Error: Could not allocate frame buffer\n");
    return 1;
  }
  
  double progress_start_time = GetTime();
  double total_bg_time = 0, total_render_time = 0, total_encode_time = 0;
  
  while (!WindowShouldClose() && frame_idx < FRAME_COUNT) {
    TIMING_START(total_frame);
    float deltaTime;
    if (renderMode) {
      // Use fixed time step for consistent 60fps output video
      deltaTime = 1.0f / FPS;
      currentTime = frame_idx * deltaTime;
    } else {
      // Use real time for interactive mode
      deltaTime = GetFrameTime();
      currentTime += deltaTime;
    }
    speakerTimer += deltaTime;

    // Find current caption and speaker based on time
    Character newSpeaker = currentSpeaker;
    Caption *currentCaptionData = NULL;

    for (int i = 0; i < captionCount; i++) {
      if (currentTime >= captions[i].startTime &&
          currentTime <= captions[i].endTime) {
        newSpeaker = captions[i].speaker;
        currentCaptionData = &captions[i];
        break;
      }
    }

    // Update speaker if changed
    if (newSpeaker != currentSpeaker) {
      currentSpeaker = newSpeaker;
      speakerTimer = 0.0f;
    }

    // Update character positions based on current speaker
    if (currentSpeaker == PETER) {
      if (!peter.isVisible) {
        peter.isVisible = true;
        peter.isSliding = true;
        peter.slideProgress = 0.0f;
        peter.startX = peter.x;
        peter.alpha = 1.0f;
      }
      if (stewie.isVisible) {
        stewie.isVisible = false;
        stewie.isFading = true;
      }
    } else {
      if (!stewie.isVisible) {
        stewie.isVisible = true;
        stewie.isSliding = true;
        stewie.slideProgress = 0.0f;
        stewie.startX = stewie.x;
        stewie.alpha = 1.0f;
      }
      if (peter.isVisible) {
        peter.isVisible = false;
        peter.isFading = true;
      }
    }

    // Animate Peter with easing curve
    if (peter.isSliding) {
      peter.slideProgress += deltaTime * 3.0f; // Control slide speed
      if (peter.slideProgress >= 1.0f) {
        peter.slideProgress = 1.0f;
        peter.isSliding = false;
      }
      // Ease-out cubic curve for smooth deceleration
      float t = peter.slideProgress;
      float eased = 1.0f - powf(1.0f - t, 3.0f);
      peter.x = peter.startX + (peter.targetX - peter.startX) * eased;
    }
    if (peter.isFading) {
      peter.alpha -= deltaTime * 3.0f;
      if (peter.alpha <= 0.0f) {
        peter.alpha = 0.0f;
        peter.isFading = false;
        peter.x = -peterWidth;
      }
    }

    // Animate Stewie with easing curve
    if (stewie.isSliding) {
      stewie.slideProgress += deltaTime * 3.0f; // Control slide speed
      if (stewie.slideProgress >= 1.0f) {
        stewie.slideProgress = 1.0f;
        stewie.isSliding = false;
      }
      // Ease-out cubic curve for smooth deceleration
      float t = stewie.slideProgress;
      float eased = 1.0f - powf(1.0f - t, 3.0f);
      stewie.x = stewie.startX + (stewie.targetX - stewie.startX) * eased;
    }
    if (stewie.isFading) {
      stewie.alpha -= deltaTime * 3.0f;
      if (stewie.alpha <= 0.0f) {
        stewie.alpha = 0.0f;
        stewie.isFading = false;
        stewie.x = WIDTH;
      }
    }

    BeginDrawing();

    if (renderMode && backgroundBuffer) {
      TIMING_START(background_frame);
      // Get background video frame
      if (getBackgroundFrame(&bgVideo, currentTime, backgroundBuffer) == 0) {
        // Initialize texture once, then just update data
        if (bgTexture.id == 0) {
          Image bgImage = {.data = backgroundBuffer,
                           .width = WIDTH,
                           .height = HEIGHT,
                           .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
                           .mipmaps = 1};
          bgTexture = LoadTextureFromImage(bgImage);
        } else {
          // Much faster than recreating texture
          UpdateTexture(bgTexture, backgroundBuffer);
        }
        DrawTexture(bgTexture, 0, 0, WHITE);
        total_bg_time += get_time_ms() - timing_start_background_frame;
      } else {
        ClearBackground(DARKBLUE);
      }
    } else if (renderMode) {
      ClearBackground(DARKBLUE);
    } else {
      ClearBackground(RAYWHITE);
    }

    // Draw characters at bottom of screen (100px from bottom, aligned to same
    // baseline)
    int characterBottomY = HEIGHT - 100;
    int peterY = characterBottomY - peterHeight;
    int stewieY = characterBottomY - stewieHeight;

    if (peter.x > -peterWidth && peter.x < WIDTH && peter.alpha > 0.0f) {
      Color peterTint = {255, 255, 255, (unsigned char)(peter.alpha * 255)};
      if (peterTexture.id != 0) {
        DrawTextureEx(peterTexture, (Vector2){peter.x, peterY}, 0.0f,
                      CHARACTER_SCALE, peterTint);
      } else {
        Color rectColor = {0, 0, 255, (unsigned char)(peter.alpha * 255)};
        DrawRectangle(peter.x, peterY, peterWidth, peterHeight, rectColor);
        DrawText("PETER", peter.x + 50, peterY + peterHeight / 2, 40,
                 peterTint);
      }
    }

    if (stewie.x > -stewieWidth && stewie.x < WIDTH && stewie.alpha > 0.0f) {
      Color stewieTint = {255, 255, 255, (unsigned char)(stewie.alpha * 255)};
      if (stewieTexture.id != 0) {
        DrawTextureEx(stewieTexture, (Vector2){stewie.x, stewieY}, 0.0f,
                      CHARACTER_SCALE, stewieTint);
      } else {
        Color rectColor = {0, 255, 0, (unsigned char)(stewie.alpha * 255)};
        DrawRectangle(stewie.x, stewieY, stewieWidth, stewieHeight, rectColor);
        DrawText("STEWIE", stewie.x + 50, stewieY + stewieHeight / 2, 40,
                 stewieTint);
      }
    }

    // Draw captions with word highlighting
    if (currentCaptionData && currentCaptionData->wordCount > 0) {
      int fontSize = 72; // Increased font size
      int currentWordIdx = -1;

      // Find current word being spoken
      for (int i = 0; i < currentCaptionData->wordCount; i++) {
        if (currentTime >= currentCaptionData->words[i].start &&
            currentTime <= currentCaptionData->words[i].end) {
          currentWordIdx = i;
          break;
        }
      }

      // If no word is currently being spoken, find the next upcoming word
      if (currentWordIdx == -1) {
        for (int i = 0; i < currentCaptionData->wordCount; i++) {
          if (currentTime < currentCaptionData->words[i].start) {
            currentWordIdx = i;
            break;
          }
        }
      }

      // If still no word found, use the last word if we're past the end
      if (currentWordIdx == -1 &&
          currentTime >= currentCaptionData->startTime) {
        currentWordIdx = currentCaptionData->wordCount - 1;
      }

      if (currentWordIdx >= 0) {
        // Calculate which group of 3 words to show based on current word
        int groupStart = (currentWordIdx / 3) * 3;
        int groupEnd = groupStart + 2;
        if (groupEnd >= currentCaptionData->wordCount) {
          groupEnd = currentCaptionData->wordCount - 1;
        }

        // Build display text for this group
        char displayWords[3][64];
        int wordsInGroup = 0;
        for (int i = groupStart; i <= groupEnd; i++) {
          strncpy(displayWords[wordsInGroup], currentCaptionData->words[i].word,
                  63);
          displayWords[wordsInGroup][63] = '\0';
          wordsInGroup++;
        }

        // Calculate total width for centering
        float totalWidth = 0;
        for (int i = 0; i < wordsInGroup; i++) {
          Vector2 wordSize =
              MeasureTextEx(boldFont, displayWords[i], fontSize, 1);
          totalWidth += wordSize.x;
          if (i < wordsInGroup - 1) {
            Vector2 spaceSize = MeasureTextEx(boldFont, " ", fontSize, 1);
            totalWidth += spaceSize.x;
          }
        }

        int textX = (WIDTH - totalWidth) / 2;
        int textY = (HEIGHT - fontSize) / 2; // Vertical center

        // Draw words individually with black outline and highlighting
        float xOffset = 0;
        for (int i = 0; i < wordsInGroup; i++) {
          int wordIdx = groupStart + i;
          Color wordColor = WHITE;

          // Highlight if this word is currently being spoken
          if (currentTime >= currentCaptionData->words[wordIdx].start &&
              currentTime <= currentCaptionData->words[wordIdx].end) {
            wordColor = GREEN;
          }

          Vector2 wordPos = {textX + xOffset, textY};

          // Draw black outline by drawing text in 8 directions
          int outlineSize = 2; // Reduced outline size for cleaner look
          for (int ox = -outlineSize; ox <= outlineSize; ox++) {
            for (int oy = -outlineSize; oy <= outlineSize; oy++) {
              if (ox != 0 || oy != 0) {
                DrawTextEx(boldFont, displayWords[i],
                           (Vector2){wordPos.x + ox, wordPos.y + oy}, fontSize,
                           1, BLACK); // Reduced spacing for sharper text
              }
            }
          }

          // Draw main text
          DrawTextEx(boldFont, displayWords[i], wordPos, fontSize, 1,
                     wordColor);

          Vector2 wordSize =
              MeasureTextEx(boldFont, displayWords[i], fontSize, 1);
          xOffset += wordSize.x;

          // Add space between words
          if (i < wordsInGroup - 1) {
            Vector2 spaceSize = MeasureTextEx(boldFont, " ", fontSize, 1);
            xOffset += spaceSize.x;
          }
        }
      }
    }

    EndDrawing();
    
    // Only capture frame in render mode for performance
    if (renderMode) {
      TIMING_START(encode_frame);
      // Direct OpenGL pixel read - much faster than LoadImageFromScreen()
      unsigned char *pixels = rlReadScreenPixels(WIDTH, HEIGHT);
      if (pixels) {
        memcpy(rgba_frame_buffer, pixels, WIDTH * HEIGHT * 4);
        RL_FREE(pixels);  // Free the pixel data returned by rlReadScreenPixels
      }
      
      // Convert RGBA to YUV using pre-allocated buffer
      const uint8_t *in_data[1] = {rgba_frame_buffer};
      int in_linesize[1] = {4 * WIDTH};
      sws_scale(sws_ctx, in_data, in_linesize, 0, HEIGHT, video_frame->data,
                video_frame->linesize);

      video_frame->pts = frame_idx;
      AVPacket *pkt = av_packet_alloc();
      if (avcodec_send_frame(video_codec_ctx, video_frame) >= 0) {
        while (avcodec_receive_packet(video_codec_ctx, pkt) >= 0) {
          av_packet_rescale_ts(pkt, video_codec_ctx->time_base,
                               video_st->time_base);
          pkt->stream_index = video_st->index;
          av_interleaved_write_frame(fmt_ctx, pkt);
          av_packet_unref(pkt);
        }
      }
      av_packet_free(&pkt);
      total_encode_time += get_time_ms() - timing_start_encode_frame;
    }

    // Generate audio samples for this frame if audio codec is available
    if (audio_codec_ctx && renderMode) {
      int frame_size = audio_codec_ctx->frame_size;
      int samples_this_frame = (int)(deltaTime * 44100 + 0.5f);
      
      // Clear pre-allocated buffers instead of allocating new ones
      memset(temp_left, 0, samples_this_frame * sizeof(float));
      memset(temp_right, 0, samples_this_frame * sizeof(float));
      for (int i = 0; i < captionCount && i < audioFileCount; i++) {
        if (currentTime >= captions[i].startTime &&
            currentTime <= captions[i].endTime) {
          AudioFile *af = &audioFiles[i];
          if (!af->stereo_buffer || af->buffer_samples == 0)
            continue;
          double audio_time = currentTime - captions[i].startTime;
          int sample_offset = (int)(audio_time * 44100.0);
          for (int s = 0; s < samples_this_frame; s++) {
            int buffer_idx = sample_offset + s;
            if (buffer_idx >= 0 && buffer_idx < af->buffer_samples) {
              float left = af->stereo_buffer[buffer_idx * 2];
              float right = af->stereo_buffer[buffer_idx * 2 + 1];
              left = fmaxf(-1.0f, fminf(1.0f, left));
              right = fmaxf(-1.0f, fminf(1.0f, right));
              temp_left[s] = left * 0.9f;
              temp_right[s] = right * 0.9f;
            }
          }
          break;
        }
      }
      // Check buffer bounds before copying
      if (audio_buffer_len + samples_this_frame < audio_buffer_size) {
        memcpy(audio_buffer_left + audio_buffer_len, temp_left,
               samples_this_frame * sizeof(float));
        memcpy(audio_buffer_right + audio_buffer_len, temp_right,
               samples_this_frame * sizeof(float));
        audio_buffer_len += samples_this_frame;
      }
      while (audio_buffer_len >= frame_size) {
        audio_frame = av_frame_alloc();
        audio_frame->format = AV_SAMPLE_FMT_FLTP;
        audio_frame->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        audio_frame->sample_rate = 44100;
        audio_frame->nb_samples = frame_size;
        av_frame_get_buffer(audio_frame, 0);
        memcpy(audio_frame->data[0], audio_buffer_left,
               frame_size * sizeof(float));
        memcpy(audio_frame->data[1], audio_buffer_right,
               frame_size * sizeof(float));
        audio_frame->pts = audio_sample_count;
        audio_sample_count += frame_size;
        if (avcodec_send_frame(audio_codec_ctx, audio_frame) >= 0) {
          AVPacket *audio_pkt = av_packet_alloc();
          while (avcodec_receive_packet(audio_codec_ctx, audio_pkt) >= 0) {
            av_packet_rescale_ts(audio_pkt, audio_codec_ctx->time_base,
                                 audio_st->time_base);
            audio_pkt->stream_index = audio_st->index;
            av_interleaved_write_frame(fmt_ctx, audio_pkt);
            av_packet_unref(audio_pkt);
          }
          av_packet_free(&audio_pkt);
        }
        av_frame_free(&audio_frame);
        memmove(audio_buffer_left, audio_buffer_left + frame_size,
                (audio_buffer_len - frame_size) * sizeof(float));
        memmove(audio_buffer_right, audio_buffer_right + frame_size,
                (audio_buffer_len - frame_size) * sizeof(float));
        audio_buffer_len -= frame_size;
      }
    }

    double frame_total_time = get_time_ms() - timing_start_total_frame;
    total_render_time += frame_total_time - (renderMode ? (total_bg_time + total_encode_time) : 0);
    
    frame_idx++;

    // Progress reporting for render mode (less frequent for better performance)
    if (renderMode && frame_idx % 600 == 0 && frame_idx > 0) { // Every 10 seconds
      double current_time = GetTime();
      double time_elapsed = current_time - progress_start_time;
      float avg_fps = 600.0f / time_elapsed;
      progress_start_time = current_time;
      float progress = (float)frame_idx / FRAME_COUNT * 100.0f;
      
      double avg_bg = total_bg_time / frame_idx;
      double avg_render = total_render_time / frame_idx;
      double avg_encode = total_encode_time / frame_idx;
      double avg_total = frame_total_time;
      
      printf("Progress: %.1f%% (%d/%d frames) - %.1f fps\n", progress, frame_idx, FRAME_COUNT, avg_fps);
      printf("  Timing - BG: %.2fms, Render: %.2fms, Encode: %.2fms, Total: %.2fms\n",
             avg_bg, avg_render, avg_encode, avg_total);
    }
  }

  // Flush video encoder
  if (renderMode) {
    avcodec_send_frame(video_codec_ctx, NULL);
    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(video_codec_ctx, pkt) >= 0) {
      av_packet_rescale_ts(pkt, video_codec_ctx->time_base, video_st->time_base);
      pkt->stream_index = video_st->index;
      av_interleaved_write_frame(fmt_ctx, pkt);
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
  }

  if (audio_codec_ctx && audio_buffer_len > 0) {
    int frame_size = audio_codec_ctx->frame_size;
    int padding = frame_size - audio_buffer_len;
    memset(audio_buffer_left + audio_buffer_len, 0, padding * sizeof(float));
    memset(audio_buffer_right + audio_buffer_len, 0, padding * sizeof(float));
    audio_frame = av_frame_alloc();
    audio_frame->format = AV_SAMPLE_FMT_FLTP;
    audio_frame->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    audio_frame->sample_rate = 44100;
    audio_frame->nb_samples = frame_size;
    av_frame_get_buffer(audio_frame, 0);
    memcpy(audio_frame->data[0], audio_buffer_left, frame_size * sizeof(float));
    memcpy(audio_frame->data[1], audio_buffer_right,
           frame_size * sizeof(float));
    audio_frame->pts = audio_sample_count;
    audio_sample_count += frame_size;
    if (avcodec_send_frame(audio_codec_ctx, audio_frame) >= 0) {
      AVPacket *final_audio_pkt = av_packet_alloc();
      while (avcodec_receive_packet(audio_codec_ctx, final_audio_pkt) >= 0) {
        av_packet_rescale_ts(final_audio_pkt, audio_codec_ctx->time_base,
                             audio_st->time_base);
        final_audio_pkt->stream_index = audio_st->index;
        av_interleaved_write_frame(fmt_ctx, final_audio_pkt);
        av_packet_unref(final_audio_pkt);
      }
      av_packet_free(&final_audio_pkt);
    }
    av_frame_free(&audio_frame);
  }
  // Packet cleanup moved to individual scopes

  av_write_trailer(fmt_ctx);
  avio_closep(&fmt_ctx->pb);

  // Cleanup pre-allocated buffers
  if (rgba_frame_buffer)
    free(rgba_frame_buffer);
  if (audio_buffer_left)
    free(audio_buffer_left);
  if (audio_buffer_right)
    free(audio_buffer_right);
  if (temp_left)
    free(temp_left);
  if (temp_right)
    free(temp_right);
  
  // Cleanup FFmpeg resources
  if (video_codec_ctx)
    avcodec_free_context(&video_codec_ctx);
  if (audio_codec_ctx)
    avcodec_free_context(&audio_codec_ctx);
  avformat_free_context(fmt_ctx);
  if (video_frame)
    av_frame_free(&video_frame);
  if (audio_frame)
    av_frame_free(&audio_frame);
  if (sws_ctx)
    sws_freeContext(sws_ctx);

  // Cleanup background video and audio
  if (renderMode) {
    if (bgTexture.id != 0)
      UnloadTexture(bgTexture);
    cleanupBackgroundVideo(&bgVideo);
    if (backgroundBuffer)
      free(backgroundBuffer);
    // Audio buffers are now freed in main cleanup
    for (int i = 0; i < audioFileCount; i++) {
      if (audioFiles[i].stereo_buffer)
        free(audioFiles[i].stereo_buffer);
      if (audioFiles[i].swr_ctx)
        swr_free(&audioFiles[i].swr_ctx);
      if (audioFiles[i].codec_ctx)
        avcodec_free_context(&audioFiles[i].codec_ctx);
      if (audioFiles[i].frame)
        av_frame_free(&audioFiles[i].frame);
      if (audioFiles[i].pkt)
        av_packet_free(&audioFiles[i].pkt);
      if (audioFiles[i].fmt_ctx)
        avformat_close_input(&audioFiles[i].fmt_ctx);
    }
    free(audioFiles);
  }

  // Cleanup textures and font
  if (peterTexture.id != 0)
    UnloadTexture(peterTexture);
  if (stewieTexture.id != 0)
    UnloadTexture(stewieTexture);
  if (boldFont.texture.id != GetFontDefault().texture.id)
    UnloadFont(boldFont);

  CloseWindow();
  return 0;
}
