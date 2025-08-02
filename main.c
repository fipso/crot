#include <raylib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <dirent.h>

#define WIDTH 1080
#define HEIGHT 1920
#define FPS 60
#define SLIDE_SPEED 20.0f
#define CHARACTER_SCALE 0.5f
#define MAX_CAPTIONS 1000
#define MAX_TEXT_LENGTH 512

typedef enum {
    PETER,
    STEWIE
} Character;

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

// JSON parser for caption files
int loadCaptions(const char* projectId, Caption* captions, int maxCaptions) {
    char dirPath[256];
    snprintf(dirPath, sizeof(dirPath), "media/captions/%s", projectId);
    
    DIR* dir = opendir(dirPath);
    if (!dir) {
        printf("Warning: Could not open captions directory: %s\n", dirPath);
        return 0;
    }
    
    // Get all JSON files and sort them by filename
    struct dirent* entries[1000];
    int fileCount = 0;
    struct dirent* entry;
    
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
                struct dirent* temp = entries[i];
                entries[i] = entries[j];
                entries[j] = temp;
            }
        }
    }
    
    int captionCount = 0;
    float currentTimeOffset = 0.0f;
    
    for (int fileIdx = 0; fileIdx < fileCount && captionCount < maxCaptions; fileIdx++) {
        char filePath[512];
        snprintf(filePath, sizeof(filePath), "%s/%s", dirPath, entries[fileIdx]->d_name);
        
        FILE* file = fopen(filePath, "r");
        if (!file) continue;
        
        // Read entire file
        fseek(file, 0, SEEK_END);
        long fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);
        
        char* jsonContent = malloc(fileSize + 1);
        fread(jsonContent, 1, fileSize, file);
        jsonContent[fileSize] = '\0';
        fclose(file);
        
        // Extract transcript
        char* transcriptStart = strstr(jsonContent, "\"transcript\": \"");
        if (transcriptStart) {
            transcriptStart += 15; // Skip "transcript": "
            char* transcriptEnd = strstr(transcriptStart, "\",");
            if (transcriptEnd) {
                int transcriptLen = transcriptEnd - transcriptStart;
                if (transcriptLen < MAX_TEXT_LENGTH - 1) {
                    strncpy(captions[captionCount].text, transcriptStart, transcriptLen);
                    captions[captionCount].text[transcriptLen] = '\0';
                    
                    // Determine speaker from filename
                    if (strstr(entries[fileIdx]->d_name, "peter")) {
                        captions[captionCount].speaker = PETER;
                    } else if (strstr(entries[fileIdx]->d_name, "stewie")) {
                        captions[captionCount].speaker = STEWIE;
                    }
                    
                    // Parse word timing data
                    captions[captionCount].wordCount = 0;
                    char* wordsStart = strstr(jsonContent, "\"words\": [");
                    if (wordsStart) {
                        char* wordPtr = wordsStart;
                        int wordIdx = 0;
                        
                        while ((wordPtr = strstr(wordPtr, "\"word\": \"")) != NULL && wordIdx < 100) {
                            wordPtr += 9; // Skip "word": "
                            char* wordEnd = strstr(wordPtr, "\"");
                            if (wordEnd) {
                                int wordLen = wordEnd - wordPtr;
                                if (wordLen < 63) {
                                    strncpy(captions[captionCount].words[wordIdx].word, wordPtr, wordLen);
                                    captions[captionCount].words[wordIdx].word[wordLen] = '\0';
                                    
                                    // Find start and end times for this word
                                    char* startPtr = strstr(wordEnd, "\"start\": ");
                                    char* endPtr = strstr(wordEnd, "\"end\": ");
                                    if (startPtr && endPtr) {
                                        startPtr += 9;
                                        endPtr += 7;
                                        // Add time offset to sequence the conversations
                                        captions[captionCount].words[wordIdx].start = atof(startPtr) + currentTimeOffset;
                                        captions[captionCount].words[wordIdx].end = atof(endPtr) + currentTimeOffset;
                                        wordIdx++;
                                    }
                                }
                            }
                            wordPtr = wordEnd;
                        }
                        captions[captionCount].wordCount = wordIdx;
                    }
                    
                    // Set overall timing based on first and last word
                    if (captions[captionCount].wordCount > 0) {
                        captions[captionCount].startTime = captions[captionCount].words[0].start;
                        captions[captionCount].endTime = captions[captionCount].words[captions[captionCount].wordCount - 1].end;
                        
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
            }
        }
        
        free(jsonContent);
        free(entries[fileIdx]);
    }
    
    printf("Loaded %d captions from %s (total duration: %.1fs)\n", captionCount, dirPath, currentTimeOffset);
    
    // Return the total duration as a special value in the last caption's endTime
    if (captionCount > 0) {
        // Store total duration for later use
        captions[captionCount - 1].endTime = currentTimeOffset - 0.5f; // Remove the last gap
    }
    
    return captionCount;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <projectId>\n", argv[0]);
        return 1;
    }
    
    const char* projectId = argv[1];
    InitWindow(WIDTH, HEIGHT, "Peter & Stewie TikTok Format");
    SetTargetFPS(FPS);

    // Load font at high resolution
    Font boldFont = LoadFontEx("./media/theboldfont.ttf", 128, NULL, 0); // Load at 128px
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
    printf("Video duration: %.1f seconds (%d frames)\n", totalDuration, FRAME_COUNT);

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
    int peterWidth = peterTexture.id != 0 ? (int)(peterTexture.width * CHARACTER_SCALE) : 400;
    int peterHeight = peterTexture.id != 0 ? (int)(peterTexture.height * CHARACTER_SCALE) : 600;
    int stewieWidth = stewieTexture.id != 0 ? (int)(stewieTexture.width * CHARACTER_SCALE) : 400;
    int stewieHeight = stewieTexture.id != 0 ? (int)(stewieTexture.height * CHARACTER_SCALE) : 600;

    // Character states - positioned at bottom, Peter stays left, Stewie stays right
    CharacterState peter = {-peterWidth, 50, -peterWidth, 0.0f, 0.0f, false, false, false};
    CharacterState stewie = {WIDTH, WIDTH - stewieWidth - 50, WIDTH, 0.0f, 0.0f, false, false, false};
    
    Character currentSpeaker = PETER;
    float speakerTimer = 0.0f;
    const float SPEAKER_DURATION = 5.0f;
    float currentTime = 0.0f;

    // FFmpeg setup
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVStream *video_st = NULL;
    AVFrame *frame = NULL;
    struct SwsContext *sws_ctx = NULL;

    avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, "output.mp4");
    if (!fmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        return 1;
    }

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        return 1;
    }

    video_st = avformat_new_stream(fmt_ctx, codec);
    codec_ctx = avcodec_alloc_context3(codec);
    codec_ctx->bit_rate = 4000000;
    codec_ctx->width = WIDTH;
    codec_ctx->height = HEIGHT;
    codec_ctx->time_base = (AVRational){1, FPS};
    codec_ctx->framerate = (AVRational){FPS, 1};
    codec_ctx->gop_size = 10;
    codec_ctx->max_b_frames = 1;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return 1;
    }
    avcodec_parameters_from_context(video_st->codecpar, codec_ctx);

    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt_ctx->pb, "output.mp4", AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open output file\n");
            return 1;
        }
    }
    avformat_write_header(fmt_ctx, NULL);

    frame = av_frame_alloc();
    frame->format = codec_ctx->pix_fmt;
    frame->width = codec_ctx->width;
    frame->height = codec_ctx->height;
    av_frame_get_buffer(frame, 0);

    sws_ctx = sws_getContext(WIDTH, HEIGHT, AV_PIX_FMT_RGBA, WIDTH, HEIGHT,
                             AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL,
                             NULL);

    int frame_idx = 0;
    while (!WindowShouldClose() && frame_idx < FRAME_COUNT) {
        float deltaTime = GetFrameTime();
        currentTime += deltaTime;
        speakerTimer += deltaTime;
        
        // Find current caption and speaker based on time
        Character newSpeaker = currentSpeaker;
        Caption* currentCaptionData = NULL;
        
        for (int i = 0; i < captionCount; i++) {
            if (currentTime >= captions[i].startTime && currentTime <= captions[i].endTime) {
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
        ClearBackground(RAYWHITE);

        // Draw characters at bottom of screen (100px from bottom, aligned to same baseline)
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
                DrawText("PETER", peter.x + 50, peterY + peterHeight/2, 40, peterTint);
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
                DrawText("STEWIE", stewie.x + 50, stewieY + stewieHeight/2, 40, stewieTint);
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
            if (currentWordIdx == -1 && currentTime >= currentCaptionData->startTime) {
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
                    strncpy(displayWords[wordsInGroup], currentCaptionData->words[i].word, 63);
                    displayWords[wordsInGroup][63] = '\0';
                    wordsInGroup++;
                }
                
                // Calculate total width for centering
                float totalWidth = 0;
                for (int i = 0; i < wordsInGroup; i++) {
                    Vector2 wordSize = MeasureTextEx(boldFont, displayWords[i], fontSize, 1);
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
                                          (Vector2){wordPos.x + ox, wordPos.y + oy}, 
                                          fontSize, 1, BLACK); // Reduced spacing for sharper text
                            }
                        }
                    }
                    
                    // Draw main text
                    DrawTextEx(boldFont, displayWords[i], wordPos, fontSize, 1, wordColor);
                    
                    Vector2 wordSize = MeasureTextEx(boldFont, displayWords[i], fontSize, 1);
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
        sws_scale(sws_ctx, in_data, in_linesize, 0, HEIGHT, frame->data,
                  frame->linesize);

        frame->pts = frame_idx;
        AVPacket pkt = {0};
        av_init_packet(&pkt);
        if (avcodec_send_frame(codec_ctx, frame) >= 0) {
            while (avcodec_receive_packet(codec_ctx, &pkt) >= 0) {
                av_packet_rescale_ts(&pkt, codec_ctx->time_base,
                                    video_st->time_base);
                pkt.stream_index = video_st->index;
                av_interleaved_write_frame(fmt_ctx, &pkt);
                av_packet_unref(&pkt);
            }
        }

        UnloadImage(screen);
        frame_idx++;
    }

    // Flush encoder
    avcodec_send_frame(codec_ctx, NULL);
    AVPacket pkt = {0};
    while (avcodec_receive_packet(codec_ctx, &pkt) >= 0) {
        av_packet_rescale_ts(&pkt, codec_ctx->time_base, video_st->time_base);
        pkt.stream_index = video_st->index;
        av_interleaved_write_frame(fmt_ctx, &pkt);
        av_packet_unref(&pkt);
    }

    av_write_trailer(fmt_ctx);
    avio_closep(&fmt_ctx->pb);
    avcodec_free_context(&codec_ctx);
    avformat_free_context(fmt_ctx);
    av_frame_free(&frame);
    sws_freeContext(sws_ctx);

    // Cleanup textures and font
    if (peterTexture.id != 0) UnloadTexture(peterTexture);
    if (stewieTexture.id != 0) UnloadTexture(stewieTexture);
    if (boldFont.texture.id != GetFontDefault().texture.id) UnloadFont(boldFont);

    CloseWindow();
    return 0;
}
