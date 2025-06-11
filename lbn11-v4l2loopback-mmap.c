#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#define VIDEO_DEVICE "/dev/video10"
#define BUFFER_COUNT 32

struct buffer {
    void   *start;
    size_t  length;
};

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
    int v4l2_fd = open(video_dev, O_RDWR);
    if (v4l2_fd < 0)
    {
        perror("open v4l2");
        return 1;
    }

    // Set V4L2 format
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = 4096;
    fmt.fmt.pix.height = 2048;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        perror("VIDIOC_S_FMT");
        return 1;
    }

    // Request buffers
    struct v4l2_requestbuffers req = {0};
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0)
    {
        perror("VIDIOC_REQBUFS");
        return 1;
    }

    if (req.count < BUFFER_COUNT) {
        /* You may need to free the buffers here. */
        printf("Not enough buffer memory\\n");
        exit(EXIT_FAILURE);
    }

    // Map buffers
    struct buffer buffers[BUFFER_COUNT];
    for (int i = 0; i < BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            return 1;
        }
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_WRITE, MAP_SHARED, v4l2_fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
    }

    // Prepare FFmpeg
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

    // Start streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return 1;
    }

    AVPacket pkt;
    int buf_idx = 0;
    while (av_read_frame(fmt_ctx, &pkt) >= 0)
    {
        if (pkt.stream_index == video_stream)
        {
            if (avcodec_send_packet(codec_ctx, &pkt) == 0)
            {
                while (avcodec_receive_frame(codec_ctx, frame) == 0)
                {
                    // // Instead of:
                    // sws_scale(..., yuyv_frame->data, ...);
                    // memcpy(mmap_buffer, yuyv_frame->data[0], buf_size);

                    // // Do:
                    // sws_scale(..., (uint8_t *const *)mmap_buffer, ...);

                    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, codec_ctx->height,
                              yuyv_frame->data, yuyv_frame->linesize);

                    // Copy frame to mmap buffer
                    memcpy(buffers[buf_idx].start, yuyv_frame->data[0], buf_size);

                    // Queue buffer
                    struct v4l2_buffer buf = {0};
                    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
                    buf.memory = V4L2_MEMORY_MMAP;
                    buf.index = buf_idx;
                    buf.bytesused = buf_size;
                    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
                        perror("VIDIOC_QBUF");
                        break;
                    }

                    // Dequeue buffer (wait until it's ready for reuse)
                    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
                        perror("VIDIOC_DQBUF");
                        break;
                    }

                    buf_idx = (buf_idx + 1) % BUFFER_COUNT;
                    usleep(33000); // ~30 FPS
                }
            }
        }
        av_packet_unref(&pkt);
    }

    // Stop streaming
    if (ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("VIDIOC_STREAMOFF");
    }

    // Cleanup
    av_frame_free(&frame);
    av_frame_free(&yuyv_frame);
    av_free(buffer);
    sws_freeContext(sws);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    for (int i = 0; i < BUFFER_COUNT; ++i) {
        munmap(buffers[i].start, buffers[i].length);
    }
    close(v4l2_fd);

    return 0;
}