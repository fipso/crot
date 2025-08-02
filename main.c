#include <raylib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdio.h>

#define WIDTH 1280
#define HEIGHT 720
#define FPS 30
#define DURATION 5  // seconds
#define FRAME_COUNT (FPS * DURATION)

int main(void) {
    InitWindow(WIDTH, HEIGHT, "TikTok Reel Framework");
    SetTargetFPS(FPS);

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
        BeginDrawing();
        ClearBackground(RAYWHITE);

        // Simple animation: bouncing circle
        int posY = (int)(sin(GetTime() * 4) * 200) + HEIGHT / 2;
        DrawCircle(WIDTH / 2, posY, 50, RED);
        DrawText("TikTok Reel Framework", 20, 20, 40, BLACK);

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

    CloseWindow();
    return 0;
}
