#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#define VIDEO_DEVICE "/dev/video10"

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s input.mp4\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];

    // Open output V4L2 device
    int v4l2_fd = open(VIDEO_DEVICE, O_WRONLY);
    if (v4l2_fd < 0)
    {
        perror("open v4l2");
        return 1;
    }

    // Set V4L2 format
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        perror("VIDIOC_S_FMT");
        return 1;
    }

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, filename, NULL, NULL) < 0)
    {
        fprintf(stderr, "Could not open video file\n");
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
    {
        fprintf(stderr, "Could not find stream info\n");
        return 1;
    }

    int video_stream = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++)
    {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_stream = i;
            break;
        }
    }

    if (video_stream < 0)
    {
        fprintf(stderr, "No video stream found\n");
        return 1;
    }

    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream]->codecpar;
    AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codecpar);
    avcodec_open2(codec_ctx, codec, NULL);

    struct SwsContext *sws = sws_getContext(
        codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        fmt.fmt.pix.width, fmt.fmt.pix.height, AV_PIX_FMT_YUYV422,
        SWS_BILINEAR, NULL, NULL, NULL);

    AVFrame *frame = av_frame_alloc();
    AVFrame *yuyv_frame = av_frame_alloc();
    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_YUYV422, fmt.fmt.pix.width, fmt.fmt.pix.height, 1);
    uint8_t *buffer = av_malloc(buf_size);
    av_image_fill_arrays(yuyv_frame->data, yuyv_frame->linesize, buffer, AV_PIX_FMT_YUYV422, fmt.fmt.pix.width, fmt.fmt.pix.height, 1);

    AVPacket pkt;
    while (av_read_frame(fmt_ctx, &pkt) >= 0)
    {
        if (pkt.stream_index == video_stream)
        {
            if (avcodec_send_packet(codec_ctx, &pkt) == 0)
            {
                while (avcodec_receive_frame(codec_ctx, frame) == 0)
                {
                    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, codec_ctx->height,
                              yuyv_frame->data, yuyv_frame->linesize);

                    write(v4l2_fd, yuyv_frame->data[0], buf_size);
                    usleep(33000); // ~30 FPS
                }
            }
        }
        av_packet_unref(&pkt);
    }

    // Cleanup
    av_frame_free(&frame);
    av_frame_free(&yuyv_frame);
    av_free(buffer);
    sws_freeContext(sws);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    close(v4l2_fd);

    return 0;
}
