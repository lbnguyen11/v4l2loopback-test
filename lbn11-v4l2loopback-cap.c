#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

void print_caps(__u32 caps) {
    struct {
        __u32 flag;
        const char *name;
    } cap_flags[] = {
        { V4L2_CAP_VIDEO_CAPTURE,        "VIDEO_CAPTURE" },
        { V4L2_CAP_VIDEO_OUTPUT,         "VIDEO_OUTPUT" },          // 0x00000002
        { V4L2_CAP_VIDEO_OVERLAY,        "VIDEO_OVERLAY" },
        { V4L2_CAP_VBI_CAPTURE,          "VBI_CAPTURE" },
        { V4L2_CAP_VBI_OUTPUT,           "VBI_OUTPUT" },
        { V4L2_CAP_SLICED_VBI_CAPTURE,   "SLICED_VBI_CAPTURE" },
        { V4L2_CAP_SLICED_VBI_OUTPUT,    "SLICED_VBI_OUTPUT" },
        { V4L2_CAP_RDS_CAPTURE,          "RDS_CAPTURE" },
        { V4L2_CAP_VIDEO_OUTPUT_OVERLAY, "VIDEO_OUTPUT_OVERLAY" },
        { V4L2_CAP_TUNER,                "TUNER" },
        { V4L2_CAP_AUDIO,                "AUDIO" },
        { V4L2_CAP_RADIO,                "RADIO" },
        { V4L2_CAP_MODULATOR,            "MODULATOR" },
        { V4L2_CAP_EXT_PIX_FORMAT,       "EXT_PIX_FORMAT"},         // 0x00200000
        { V4L2_CAP_READWRITE,            "READWRITE" },             // 0x01000000
        { V4L2_CAP_ASYNCIO,              "ASYNCIO" },
        { V4L2_CAP_STREAMING,            "STREAMING" },             // 0x04000000
        { V4L2_CAP_DEVICE_CAPS,          "DEVICE_CAPS" },           // 0x80000000
    };

    for (size_t i = 0; i < sizeof(cap_flags)/sizeof(cap_flags[0]); i++) {
        if (caps & cap_flags[i].flag) {
            printf("  - %s\n", cap_flags[i].name);
        }
    }
}

void test_streaming_io(int fd, enum v4l2_memory mem_type, const char *label) {
    struct v4l2_requestbuffers req = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = mem_type,
    };

    if (ioctl(fd, VIDIOC_REQBUFS, &req) == 0 && req.count > 0) {
        printf("✔ Supports streaming I/O: %s\n", label);
    } else {
        printf("✘ Does NOT support: %s (errno: %d - %s)\n", label, errno, strerror(errno));
    }

    // Clean up
    req.count = 0;
    ioctl(fd, VIDIOC_REQBUFS, &req);
}

int main(int argc, char *argv[]) {
    const char *device = "/dev/video10";
    if (argc > 1) {
        device = argv[1];
    }

    int fd = open(device, O_RDWR);
    if (fd == -1) {
        perror("Failed to open device");
        return 1;
    }

    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        perror("VIDIOC_QUERYCAP");
        close(fd);
        return 1;
    }

    printf("Device:      %s\n", device);
    printf("Driver:      %s\n", cap.driver);
    printf("Card:        %s\n", cap.card);
    printf("Bus info:    %s\n", cap.bus_info);
    printf("Version:     %u.%u.%u\n",
           (cap.version >> 16) & 0xFF,
           (cap.version >> 8) & 0xFF,
           cap.version & 0xFF);

    printf("Capabilities: 0x%08X\n", cap.capabilities);
    print_caps(cap.capabilities);

    if (cap.capabilities & V4L2_CAP_DEVICE_CAPS) {
        printf("Device caps:  0x%08X\n", cap.device_caps);
        print_caps(cap.device_caps);
    }

    if (cap.capabilities & V4L2_CAP_STREAMING) {
        printf("\n--- Probing streaming I/O support ---\n");
        test_streaming_io(fd, V4L2_MEMORY_MMAP, "V4L2_MEMORY_MMAP");
        test_streaming_io(fd, V4L2_MEMORY_USERPTR, "V4L2_MEMORY_USERPTR");
#ifdef V4L2_MEMORY_DMABUF
        test_streaming_io(fd, V4L2_MEMORY_DMABUF, "V4L2_MEMORY_DMABUF");
#endif
    } else {
        printf("Streaming I/O is not supported.\n");
    }

    close(fd);
    return 0;
}