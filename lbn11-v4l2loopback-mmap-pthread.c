#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <errno.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define MAX_BUFFERS 32

typedef struct {
    int fd;
    struct v4l2_buffer buffers[MAX_BUFFERS];
    void *buffer_start[MAX_BUFFERS];
    int buffer_used[MAX_BUFFERS];
    int buffer_count;
    pthread_mutex_t lock;
    pthread_cond_t buffer_available; // signaled by producer
    pthread_cond_t buffer_free;      // signaled by consumer
} buffer_pool_t;

typedef struct {
    buffer_pool_t *pool;
    const char *filename;
    struct v4l2_format* fmt;
} producer_arg_t;

// shared buffer pool
buffer_pool_t buffer_pool;

// return the first available buffer's index in buffer pool
int find_available_buffer(buffer_pool_t *pool) {
    for (int i = 0; i < pool->buffer_count; ++i) {
        if (!pool->buffer_used[i]) return i;
    }
    return -1;
}

// return the first used buffer's index in buffer pool
int find_used_buffer(buffer_pool_t *pool) {
    for (int i = 0; i < pool->buffer_count; ++i) {
        if (pool->buffer_used[i]) return i;
    }
    return -1;
}

// do ioctl QBUF if any buffer in the poll is available (DQBUF'ed)
void *producer_thread(void *arg) {
    producer_arg_t *parg = (producer_arg_t *)arg;
    buffer_pool_t *pool = parg->pool;
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, parg->filename, NULL, NULL) < 0) {
        fprintf(stderr, "Failed to open input file\n");
        return NULL;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Failed to find stream info\n");
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    int video_stream_index = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }
    if (video_stream_index == -1) {
        fprintf(stderr, "No video stream found\n");
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream_index]->codecpar;
    AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Failed to find decoder\n");
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        fprintf(stderr, "Failed to copy codec parameters\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    struct v4l2_format* custom_fmt = parg->fmt;
    AVFrame *frame = av_frame_alloc();
    AVFrame *yuyv_frame = av_frame_alloc();
    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_YUYV422, custom_fmt->fmt.pix.width, custom_fmt->fmt.pix.height, 1);
    printf("buf_size=%d\n", buf_size);
    uint8_t *buffer = av_malloc(buf_size);
    av_image_fill_arrays(yuyv_frame->data, yuyv_frame->linesize, buffer, AV_PIX_FMT_YUYV422, custom_fmt->fmt.pix.width, custom_fmt->fmt.pix.height, 1);

    AVPacket *pkt = av_packet_alloc();
    struct SwsContext *sws = sws_getContext(codecpar->width, codecpar->height, codec_ctx->pix_fmt,
                                            custom_fmt->fmt.pix.width, custom_fmt->fmt.pix.height, AV_PIX_FMT_YUYV422,
                                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!frame || !yuyv_frame || !buffer || !pkt || !sws) {
        fprintf(stderr, "Failed to allocate frame, packet, buffer, pkt or sws context\n");
        av_frame_free(&frame);
        av_frame_free(&yuyv_frame);
        av_free(buffer);
        av_packet_free(&pkt);
        sws_freeContext(sws);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    int stride = custom_fmt->fmt.pix.width * 2; // YUV422: 2 bytes per pixel

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_stream_index) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(codec_ctx, pkt) < 0) {
            fprintf(stderr, "Failed to send packet\n");
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            int idx = -1;
            pthread_mutex_lock(&pool->lock);
            while ((idx = find_available_buffer(pool)) < 0) {
                pthread_cond_wait(&pool->buffer_free, &pool->lock);
            }

            // Set yuyv_frame to point directly to the mmap buffer (zero-copy overhead)
            yuyv_frame->data[0] = pool->buffer_start[idx];
            yuyv_frame->linesize[0] = stride;

            sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, codecpar->height,
                      yuyv_frame->data, yuyv_frame->linesize);

            struct v4l2_buffer buf = {
                .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                .index = idx,
                .memory = V4L2_MEMORY_MMAP,
                .bytesused = buf_size
            };

            if (ioctl(pool->fd, VIDIOC_QBUF, &buf) < 0) {
                perror("ioctl VIDIOC_QBUF");
            } else {
                pool->buffer_used[idx] = 1;
                printf("producer did VIDIOC_QBUF  for buffer #%d\n", idx);
                // signal consumer thread to do ioctl DQBUF
                pthread_cond_signal(&pool->buffer_available);
            }

            pthread_mutex_unlock(&pool->lock);
            usleep(16666); // ~60 FPS
        }
    }

    av_frame_free(&frame);
    av_frame_free(&yuyv_frame);
    av_free(buffer);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return NULL;
}

// do ioctl DQBUF if any buffer in the poll is used (QBUF'ed)
void *consumer_thread(void *arg) {
    buffer_pool_t *pool = (buffer_pool_t *)arg;
    struct pollfd pfd = {
        .fd = pool->fd,
        .events = POLLOUT
    };

    while (1) {
        int idx = -1;
        pthread_mutex_lock(&pool->lock);
        while ((idx = find_used_buffer(pool)) < 0) {
            pthread_cond_wait(&pool->buffer_available, &pool->lock);
        }
        pthread_mutex_unlock(&pool->lock);

        if (poll(&pfd, 1, -1) <= 0) {
            perror("poll");
            continue;
        }

        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
            .memory = V4L2_MEMORY_MMAP
        };
        pthread_mutex_lock(&pool->lock);
        if (ioctl(pool->fd, VIDIOC_DQBUF, &buf) == 0) {
            pool->buffer_used[buf.index] = 0;
            printf("consumer did VIDIOC_DQBUF for buffer #%d\n", buf.index);
            pthread_cond_signal(&pool->buffer_free);
        } else {
            perror("ioctl VIDIOC_DQBUF");
        }
        pthread_mutex_unlock(&pool->lock);
    }

    return NULL;
}

void init_buffers(int fd, buffer_pool_t *pool) {
    // allocate device buffers for streaming I/O (memory mapping)
    struct v4l2_requestbuffers req = {
        .count = MAX_BUFFERS,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_MMAP
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("ioctl VIDIOC_REQBUFS");
        exit(EXIT_FAILURE);
    }

    if (req.count < MAX_BUFFERS) {
        printf("Not enough buffer memory\n");
        exit(EXIT_FAILURE);
    }
    pool->buffer_count = req.count;
    pool->fd = fd;

    for (int i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
            .index = i,
            .memory = V4L2_MEMORY_MMAP
        };
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("ioctl VIDIOC_QUERYBUF");
            exit(EXIT_FAILURE);
        }
        pool->buffers[i] = buf;
        // map the device buffers into application address space using mmap()
        pool->buffer_start[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (pool->buffer_start[i] == MAP_FAILED) {
            perror("mmap");
            exit(EXIT_FAILURE);
        }
        pool->buffer_used[i] = 0;
    }

    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        perror("pthread_mutex_init");
        exit(EXIT_FAILURE);
    }
    if (pthread_cond_init(&pool->buffer_available, NULL) != 0) {
        perror("pthread_cond_init buffer_available");
        exit(EXIT_FAILURE);
    }
    if (pthread_cond_init(&pool->buffer_free, NULL) != 0) {
        perror("pthread_cond_init buffer_free");
        exit(EXIT_FAILURE);
    }
}

// munmmap device buffers after used
void cleanup_buffers(buffer_pool_t *pool) {
    for (int i = 0; i < pool->buffer_count; ++i) {
        if (pool->buffer_start[i] && pool->buffers[i].length > 0) {
            munmap(pool->buffer_start[i], pool->buffers[i].length);
            pool->buffer_start[i] = NULL;
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s /dev/video<X> input.mp4\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *device = argv[1];
    const char *filename = argv[2];

    // Open V4L2 device
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("open video device");
        return EXIT_FAILURE;
    }

    // Set V4L2 format
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = 4096;
    fmt.fmt.pix.height = 4096;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        perror("VIDIOC_S_FMT");
        return EXIT_FAILURE;
    }

    init_buffers(fd, &buffer_pool);

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("ioctl VIDIOC_STREAMON");
        return EXIT_FAILURE;
    }

    pthread_t prod, cons;
    producer_arg_t parg = { .pool = &buffer_pool, .filename = filename, .fmt = &fmt};

    if (pthread_create(&prod, NULL, producer_thread, &parg) != 0) {
        perror("pthread_create producer");
        return EXIT_FAILURE;
    }
    if (pthread_create(&cons, NULL, consumer_thread, &buffer_pool) != 0) {
        perror("pthread_create consumer");
        return EXIT_FAILURE;
    }

    pthread_join(prod, NULL);
    pthread_cancel(cons);

    cleanup_buffers(&buffer_pool);
    close(fd);

    return EXIT_SUCCESS;
}