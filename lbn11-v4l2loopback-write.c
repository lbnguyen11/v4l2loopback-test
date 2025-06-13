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

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("Usage: %s /dev/video<X> input.mp4\n", argv[0]);
        return 1;
    }

    const char *video_dev = argv[1];
    const char *filename = argv[2];

    // Open output V4L2 device
    int v4l2_fd = open(video_dev, O_WRONLY);
    if (v4l2_fd < 0)
    {
        perror("open v4l2");
        return 1;
    }

    // Set V4L2 format
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = 4096;
    fmt.fmt.pix.height = 4096;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        perror("VIDIOC_S_FMT");
        close(v4l2_fd);
        return 1;
    }

    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    struct SwsContext *sws = NULL;
    AVFrame *frame = NULL, *yuyv_frame = NULL;
    uint8_t *buffer = NULL;
    int buf_size = 0;
    int ret = 1; // default to error

    if (avformat_open_input(&fmt_ctx, filename, NULL, NULL) < 0)
    {
        fprintf(stderr, "Could not open video file\n");
        goto cleanup;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
    {
        fprintf(stderr, "Could not find stream info\n");
        goto cleanup;
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
        goto cleanup;
    }

    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream]->codecpar;
    AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        fprintf(stderr, "Could not find decoder for codec_id %d\n", codecpar->codec_id);
        goto cleanup;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
    {
        fprintf(stderr, "Could not allocate codec context\n");
        goto cleanup;
    }

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0)
    {
        fprintf(stderr, "Could not copy codec parameters to context\n");
        goto cleanup;
    }

    if (avcodec_open2(codec_ctx, codec, NULL) < 0)
    {
        fprintf(stderr, "Could not open codec\n");
        goto cleanup;
    }

    sws = sws_getContext(
        codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        fmt.fmt.pix.width, fmt.fmt.pix.height, AV_PIX_FMT_YUYV422,
        SWS_BILINEAR, NULL, NULL, NULL);

    frame = av_frame_alloc();
    yuyv_frame = av_frame_alloc();
    buf_size = av_image_get_buffer_size(AV_PIX_FMT_YUYV422, fmt.fmt.pix.width, fmt.fmt.pix.height, 1);
    buffer = (buf_size > 0) ? av_malloc(buf_size) : NULL;

    if (!frame || !yuyv_frame || !buffer || !sws)
    {
        fprintf(stderr, "Failed to allocate frame, buffer, or sws context\n");
        goto cleanup;
    }

    if (av_image_fill_arrays(yuyv_frame->data, yuyv_frame->linesize, buffer, AV_PIX_FMT_YUYV422, fmt.fmt.pix.width, fmt.fmt.pix.height, 1) < 0)
    {
        fprintf(stderr, "Could not fill image arrays\n");
        goto cleanup;
    }

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

                    // call write systemcall, v4l2 driver will take care 
                    // the allocation of the device buffers and the copy of user-space data to device buffers
                    ssize_t written = write(v4l2_fd, yuyv_frame->data[0], buf_size);
                    if (written < 0)
                    {
                        perror("write to v4l2 device");
                        goto cleanup;
                    }
                    if (written != buf_size)
                    {
                        fprintf(stderr, "Partial write to v4l2 device: %zd/%d bytes\n", written, buf_size);
                        goto cleanup;
                    }
                    usleep(16666); // ~60 FPS
                }
            }
        }
        av_packet_unref(&pkt);
    }

    ret = 0; // success

cleanup:
    av_frame_free(&frame);
    av_frame_free(&yuyv_frame);
    av_free(buffer);
    sws_freeContext(sws);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    close(v4l2_fd);

    return ret;
}