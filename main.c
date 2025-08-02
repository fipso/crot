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
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 1080
#define HEIGHT 1920
#define FPS 60
#define SLIDE_SPEED 20.0f
#define CHARACTER_SCALE 0.5f
#define MAX_CAPTIONS 1000
#define MAX_TEXT_LENGTH 512

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

// Background video decoder context
typedef struct {
  AVFormatContext *fmt_ctx;
  AVCodecContext *codec_ctx;
  AVStream *video_stream;
  struct SwsContext *sws_ctx;
  AVFrame *frame;
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

// Initialize background video decoder with AMD hardware acceleration
int initBackgroundVideo(BackgroundVideo *bg, const char *filename) {
  bg->fmt_ctx = NULL;
  bg->codec_ctx = NULL;
  bg->video_stream = NULL;
  bg->sws_ctx = NULL;
  bg->frame = NULL;
  bg->pkt = NULL;
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

  // Try AMD hardware decoders first, then fallback to software
  const AVCodec *codec = NULL;
  const char *hw_decoder_names[] = {"h264_vaapi", // AMD VAAPI H.264 decoder
                                    "hevc_vaapi", // AMD VAAPI HEVC decoder
                                    "h264_amf",   // AMD AMF H.264 decoder
                                    "hevc_amf",   // AMD AMF HEVC decoder
                                    NULL};

  // First try hardware decoders based on codec type
  if (bg->video_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
    for (int i = 0; hw_decoder_names[i]; i++) {
      if (strstr(hw_decoder_names[i], "h264")) {
        codec = avcodec_find_decoder_by_name(hw_decoder_names[i]);
        if (codec) {
          printf("Using AMD hardware decoder: %s\n", hw_decoder_names[i]);
          break;
        }
      }
    }
  } else if (bg->video_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
    for (int i = 0; hw_decoder_names[i]; i++) {
      if (strstr(hw_decoder_names[i], "hevc")) {
        codec = avcodec_find_decoder_by_name(hw_decoder_names[i]);
        if (codec) {
          printf("Using AMD hardware decoder: %s\n", hw_decoder_names[i]);
          break;
        }
      }
    }
  }

  // Fallback to software decoder
  if (!codec) {
    codec = avcodec_find_decoder(bg->video_stream->codecpar->codec_id);
    if (codec) {
      printf("Using software decoder: %s\n", codec->name);
    }
  }

  if (!codec) {
    printf("Error: Unsupported codec\n");
    return -1;
  }

  // Allocate codec context
  bg->codec_ctx = avcodec_alloc_context3(codec);
  if (!bg->codec_ctx) {
    printf("Error: Could not allocate codec context\n");
    return -1;
  }

  // Copy codec parameters
  if (avcodec_parameters_to_context(bg->codec_ctx, bg->video_stream->codecpar) <
      0) {
    printf("Error: Could not copy codec parameters\n");
    return -1;
  }

  // Set hardware acceleration options for VAAPI
  if (strstr(codec->name, "vaapi")) {
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "hwaccel", "vaapi", 0);
    av_dict_set(&opts, "hwaccel_device", "/dev/dri/renderD128", 0);

    if (avcodec_open2(bg->codec_ctx, codec, &opts) < 0) {
      printf("Warning: Could not open hardware decoder, trying software "
             "fallback\n");
      av_dict_free(&opts);
      avcodec_free_context(&bg->codec_ctx);

      // Fallback to software decoder
      codec = avcodec_find_decoder(bg->video_stream->codecpar->codec_id);
      if (!codec) {
        printf("Error: Could not find software decoder\n");
        return -1;
      }
      bg->codec_ctx = avcodec_alloc_context3(codec);
      avcodec_parameters_to_context(bg->codec_ctx, bg->video_stream->codecpar);
      if (avcodec_open2(bg->codec_ctx, codec, NULL) < 0) {
        printf("Error: Could not open software decoder\n");
        return -1;
      }
      printf("Using software decoder fallback: %s\n", codec->name);
    } else {
      printf("Successfully initialized AMD VAAPI hardware decoder\n");
    }
    av_dict_free(&opts);
  } else {
    // Open codec normally
    if (avcodec_open2(bg->codec_ctx, codec, NULL) < 0) {
      printf("Error: Could not open codec\n");
      return -1;
    }
  }

  // Allocate frame and packet
  bg->frame = av_frame_alloc();
  bg->pkt = av_packet_alloc();

  if (!bg->frame || !bg->pkt) {
    printf("Error: Could not allocate frame or packet\n");
    return -1;
  }

  // Calculate proper scaling dimensions to fit 9:16 aspect ratio
  int src_width = bg->codec_ctx->width;
  int src_height = bg->codec_ctx->height;
  float src_aspect = (float)src_width / src_height;
  float target_aspect = (float)WIDTH / HEIGHT; // 9:16 = 0.5625

  int scaled_width, scaled_height;
  if (src_aspect > target_aspect) {
    // Source is wider - fit to height and crop sides
    scaled_height = HEIGHT;
    scaled_width = (int)(HEIGHT * src_aspect);
  } else {
    // Source is taller - fit to width and crop top/bottom
    scaled_width = WIDTH;
    scaled_height = (int)(WIDTH / src_aspect);
  }

  // Setup scaling context with proper dimensions
  bg->sws_ctx = sws_getContext(src_width, src_height, bg->codec_ctx->pix_fmt,
                               scaled_width, scaled_height, AV_PIX_FMT_RGBA,
                               SWS_BILINEAR, NULL, NULL, NULL);

  if (!bg->sws_ctx) {
    printf("Error: Could not initialize scaling context\n");
    return -1;
  }

  bg->time_base = av_q2d(bg->video_stream->time_base);
  bg->start_time = bg->video_stream->start_time;

  printf("Background video initialized: %dx%d, time_base: %f\n",
         bg->codec_ctx->width, bg->codec_ctx->height, bg->time_base);

  return 0;
}

// Get background video frame at specific time
int getBackgroundFrame(BackgroundVideo *bg, double target_time,
                       uint8_t *rgba_buffer) {
  // Clear the buffer to black first
  memset(rgba_buffer, 0, WIDTH * HEIGHT * 4);

  // Calculate target PTS for seeking
  int64_t target_pts = (int64_t)(target_time / bg->time_base);
  if (bg->start_time != AV_NOPTS_VALUE) {
    target_pts += bg->start_time;
  }

  // Only seek when we have a significant time jump
  static int64_t last_pts = -1;
  static double last_frame_time = -1.0;

  // Clear the buffer to black first
  memset(rgba_buffer, 0, WIDTH * HEIGHT * 4);

  // Calculate target PTS for seeking
  if (bg->start_time != AV_NOPTS_VALUE) {
    target_pts += bg->start_time;
  }

  // Seek if first time or significant jump or going backwards
  if (last_pts == -1 || target_pts < last_pts || llabs(target_pts - last_pts) > (int64_t)(0.5 / bg->time_base)) {
    int seek_flags = AVSEEK_FLAG_BACKWARD;
    if (av_seek_frame(bg->fmt_ctx, bg->stream_index, target_pts, seek_flags) >= 0) {
      avcodec_flush_buffers(bg->codec_ctx);
    }
    last_pts = target_pts;
  }

  // Read frames until we find one close to our target time
  while (av_read_frame(bg->fmt_ctx, bg->pkt) >= 0) {
    if (bg->pkt->stream_index == bg->stream_index) {
      if (avcodec_send_packet(bg->codec_ctx, bg->pkt) >= 0) {
        while (avcodec_receive_frame(bg->codec_ctx, bg->frame) >= 0) {
          int64_t frame_pts = bg->frame->pts;
          if (bg->start_time != AV_NOPTS_VALUE) {
            frame_pts -= bg->start_time;
          }

          double frame_time = frame_pts * bg->time_base;

          // Skip if frame is before last frame time (prevent jumping back)
          if (frame_time < last_frame_time) continue;

          // Use frame if it's the closest before or at target
          if (frame_time <= target_time + 0.02) {
            last_frame_time = frame_time;
            // Calculate scaled dimensions
            int src_width = bg->codec_ctx->width;
            int src_height = bg->codec_ctx->height;
            float src_aspect = (float)src_width / src_height;
            float target_aspect = (float)WIDTH / HEIGHT;

            int scaled_width, scaled_height;
            if (src_aspect > target_aspect) {
              scaled_height = HEIGHT;
              scaled_width = (int)(HEIGHT * src_aspect);
            } else {
              scaled_width = WIDTH;
              scaled_height = (int)(WIDTH / src_aspect);
            }

            // Create temporary buffer for scaled image
            uint8_t *temp_buffer = malloc(scaled_width * scaled_height * 4);
            if (!temp_buffer)
              return -1;

            uint8_t *temp_data[1] = {temp_buffer};
            int temp_linesize[1] = {scaled_width * 4};

            // Scale to temporary buffer
            sws_scale(bg->sws_ctx, (const uint8_t *const *)bg->frame->data,
                      bg->frame->linesize, 0, src_height, temp_data,
                      temp_linesize);

            // Copy centered portion to output buffer
            int offset_x = (scaled_width - WIDTH) / 2;
            int offset_y = (scaled_height - HEIGHT) / 2;

            for (int y = 0; y < HEIGHT; y++) {
              int src_y = y + offset_y;
              if (src_y >= 0 && src_y < scaled_height) {
                uint8_t *src_line =
                    temp_buffer + (src_y * scaled_width + offset_x) * 4;
                uint8_t *dst_line = rgba_buffer + y * WIDTH * 4;
                memcpy(dst_line, src_line, WIDTH * 4);
              }
            }

            free(temp_buffer);
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

                    // Estimate buffer size (assume ~3 second audio files)
                    int estimated_samples =
                        44100 * 5; // 5 seconds stereo at 44.1kHz
                    af->stereo_buffer =
                        malloc(estimated_samples * 2 * sizeof(float));
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

                                if (converted > 0 &&
                                    af->buffer_samples + converted <
                                        estimated_samples) {
                                  float *left = (float *)out_data[0];
                                  float *right = (float *)out_data[1];

                                  // Simple interleave stereo samples
                                  for (int i = 0; i < converted; i++) {
                                    af->stereo_buffer[(af->buffer_samples + i) *
                                                      2] = left[i];
                                    af->stereo_buffer[(af->buffer_samples + i) *
                                                          2 +
                                                      1] = right[i];
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

    printf("AMD hardware acceleration initialized for render mode\n");
  }

  // Setup output format with both video and audio
  const char *outputFile = renderMode ? "output_render.mp4" : "output.mp4";
  avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, outputFile);
  if (!fmt_ctx) {
    fprintf(stderr, "Could not create output context\n");
    return 1;
  }

  // Setup video codec with optimizations
  const AVCodec *video_codec = avcodec_find_encoder_by_name("libx264");
  if (!video_codec) {
    fprintf(stderr, "libx264 encoder not found\n");
    return 1;
  }

  video_st = avformat_new_stream(fmt_ctx, video_codec);
  video_codec_ctx = avcodec_alloc_context3(video_codec);

  // Configure encoder settings for maximum speed
  video_codec_ctx->bit_rate = 6000000; // Balanced bitrate
  video_codec_ctx->width = WIDTH;
  video_codec_ctx->height = HEIGHT;
  video_codec_ctx->time_base = (AVRational){1, FPS};
  video_codec_ctx->framerate = (AVRational){FPS, 1};
  video_codec_ctx->gop_size = 60;    // Larger GOP for better compression
  video_codec_ctx->max_b_frames = 0; // Disable B-frames for speed
  video_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

  // Ultra-fast software encoding settings with maximum parallelization
  AVDictionary *encoder_opts = NULL;
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

  if (avcodec_open2(video_codec_ctx, video_codec, &encoder_opts) < 0) {
    fprintf(stderr, "Could not open video codec\n");
    av_dict_free(&encoder_opts);
    return 1;
  }
  av_dict_free(&encoder_opts);

  printf("Successfully initialized optimized software H.264 encoder\n");
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
                           AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

  static int64_t audio_sample_count = 0;
  // Audio buffer for rate matching
  float *audio_buffer_left = malloc(2048 * sizeof(float));
  float *audio_buffer_right = malloc(2048 * sizeof(float));
  int audio_buffer_len = 0;
  int frame_idx = 0;
  while (!WindowShouldClose() && frame_idx < FRAME_COUNT) {
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
      // Get background video frame
      if (getBackgroundFrame(&bgVideo, currentTime, backgroundBuffer) == 0) {
        // Create background image without loading as texture (more efficient)
        Image bgImage = {.data = backgroundBuffer,
                         .width = WIDTH,
                         .height = HEIGHT,
                         .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
                         .mipmaps = 1};

        // Reuse texture instead of creating new one each frame
        if (bgTexture.id != 0) {
          UpdateTexture(bgTexture, backgroundBuffer);
        } else {
          bgTexture = LoadTextureFromImage(bgImage);
        }
        DrawTexture(bgTexture, 0, 0, WHITE);
      } else {
        // Fallback to dark background
        ClearBackground(DARKBLUE);
      }
    } else if (renderMode) {
      // Use dark background for render mode (placeholder for background video)
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

    // Capture frame
    Image screen = LoadImageFromScreen();
    uint8_t *rgba_data = (uint8_t *)screen.data;

    // Convert RGBA to YUV
    const uint8_t *in_data[1] = {rgba_data};
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

    // Generate audio samples for this frame if audio codec is available
    if (audio_codec_ctx && renderMode) {
      int frame_size = audio_codec_ctx->frame_size;
      int samples_this_frame = (int)(deltaTime * 44100 + 0.5f);
      float *temp_left = calloc(samples_this_frame, sizeof(float));
      float *temp_right = calloc(samples_this_frame, sizeof(float));
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
      memcpy(audio_buffer_left + audio_buffer_len, temp_left,
             samples_this_frame * sizeof(float));
      memcpy(audio_buffer_right + audio_buffer_len, temp_right,
             samples_this_frame * sizeof(float));
      audio_buffer_len += samples_this_frame;
      free(temp_left);
      free(temp_right);
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
          while (avcodec_receive_packet(audio_codec_ctx, pkt) >= 0) {
            av_packet_rescale_ts(pkt, audio_codec_ctx->time_base,
                                 audio_st->time_base);
            pkt->stream_index = audio_st->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
          }
        }
        av_frame_free(&audio_frame);
        memmove(audio_buffer_left, audio_buffer_left + frame_size,
                (audio_buffer_len - frame_size) * sizeof(float));
        memmove(audio_buffer_right, audio_buffer_right + frame_size,
                (audio_buffer_len - frame_size) * sizeof(float));
        audio_buffer_len -= frame_size;
      }
    }

    av_packet_free(&pkt);

    UnloadImage(screen);
    frame_idx++;

    // Progress reporting for render mode
    if (renderMode && frame_idx % 60 == 0) { // Every second
      float progress = (float)frame_idx / FRAME_COUNT * 100.0f;
      printf("Progress: %.1f%% (%d/%d frames) - %.1f fps\n", progress,
             frame_idx, FRAME_COUNT, 60.0f / GetFrameTime());
    }
  }

  // Flush video encoder
  avcodec_send_frame(video_codec_ctx, NULL);
  AVPacket *pkt = av_packet_alloc();
  while (avcodec_receive_packet(video_codec_ctx, pkt) >= 0) {
    av_packet_rescale_ts(pkt, video_codec_ctx->time_base, video_st->time_base);
    pkt->stream_index = video_st->index;
    av_interleaved_write_frame(fmt_ctx, pkt);
    av_packet_unref(pkt);
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
      while (avcodec_receive_packet(audio_codec_ctx, pkt) >= 0) {
        av_packet_rescale_ts(pkt, audio_codec_ctx->time_base,
                             audio_st->time_base);
        pkt->stream_index = audio_st->index;
        av_interleaved_write_frame(fmt_ctx, pkt);
        av_packet_unref(pkt);
      }
    }
    av_frame_free(&audio_frame);
  }
  av_packet_free(&pkt);

  av_write_trailer(fmt_ctx);
  avio_closep(&fmt_ctx->pb);

  // Cleanup
  if (video_codec_ctx)
    avcodec_free_context(&video_codec_ctx);
  if (audio_codec_ctx)
    avcodec_free_context(&audio_codec_ctx);
  avformat_free_context(fmt_ctx);
  if (video_frame)
    av_frame_free(&video_frame);
  if (audio_frame)
    av_frame_free(&audio_frame);
  sws_freeContext(sws_ctx);

  // Cleanup background video and audio
  if (renderMode) {
    if (bgTexture.id != 0)
      UnloadTexture(bgTexture);
    cleanupBackgroundVideo(&bgVideo);
    if (backgroundBuffer)
      free(backgroundBuffer);
    if (audio_codec_ctx) {
      free(audio_buffer_left);
      free(audio_buffer_right);
    }
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
