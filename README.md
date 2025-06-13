# v4l2loopback-test

Examining the streaming I/O methods (Memory Mapping, User Pointers, DMABUF) of v4l2loopback module/driver.

## Target & Planning
- Study read()/write() support function of v4l2loopback module: done
- Study Memory Mapping streaming method of v4l2loopback module: done
- Compare buffer handling mechanism between read()/write() and Memory Mapping: done
- Add basic security access check for v4l2loopback module: done
- Experiment for optimazation of Memory Map buffer handling of v4l2loopback: in progress, estimation: 30h
- Experiment User Pointers and DMABUF methods of v4l2loopback (currently not supported): in progress, estimation: 60h

## Dependencies

* OS: Linux
* ffmpeg
* libavformat, libavcodec, libswscale

## Installing dependencies

* sudo apt update
* sudo apt install ffmpeg
* sudo apt install libavformat-dev libavcodec-dev libswscale-dev libavutil-dev

## Instructions

* Clone the repo
```
git clone https://github.com/lbnguyen11/v4l2loopback-test.git
cd v4l2loopback-test
```

* Build v4l2loopback module from source
```
make
sudo make install
```

* Load the v4l2loopback module
```
sudo modprobe v4l2loopback max_buffers=32 video_nr=10 card_label="VirtualCam" exclusive_caps=1 debug=8
// observe kernel log
sudo dmesg -w | egrep "v4l2|video_dev"
```

* Test read/write functions of v4l2loopback module 
```
gcc -g -O2 -Wall -o lbn11-v4l2loopback-write lbn11-v4l2loopback-write.c `pkg-config --cflags --libs libavformat libavcodec libswscale libavutil`
// send video's frames to /dev/video10 using write() 
sudo perf record -g -F 999 --call-graph=dwarf ./lbn11-v4l2loopback-write /dev/video10 <input.mp4>
// streaming from /dev/video10
ffplay /dev/video10
// read perf report
sudo perf report -g
```

* Test Memory Mapping streaming I/O method of v4l2loopback module 
```
gcc -g -O2 -Wall -o lbn11-v4l2loopback-mmap-pthread lbn11-v4l2loopback-mmap-pthread.c `pkg-config --cflags --libs libavformat libavcodec libswscale libavutil`
// send video's frames to /dev/video10 using Memory Mapping method
sudo perf record -g -F 999 --call-graph=dwarf ./lbn11-v4l2loopback-mmap-pthread /dev/video10 <input.mp4>
// streaming from /dev/video10
ffplay /dev/video10
// read perf report
sudo perf report -g
```

* Test Security access check for a specific GID
```
// remove the module
sudo modprobe -r v4l2loopback
// load the module again with param allowed_gid=0 (only allow access to GID 0)
sudo modprobe v4l2loopback max_buffers=32 video_nr=10 card_label="VirtualCam" exclusive_caps=1 debug=8 allowed_gid=0
// access from normal user not in GID 0, will not work (open video device: Permission denied)
./lbn11-v4l2loopback-mmap-pthread /dev/video10 <input.mp4>
// access from user with GID 0, should work
sudo ./lbn11-v4l2loopback-mmap-pthread /dev/video10 <input.mp4>
```

## Compare read/write functions and Memory Mappinng method
* read/write functions
    * v4l2 supports write() function; it can be confirmed via the V4L2_CAP_READWRITE flag in the capabilities field of struct v4l2_capability returned by the ioctl VIDIOC_QUERYCAP ioctl is set.
    * When user-space application calls write(), it passes pointer to user-space data to kernel-space module.
    * v4l2loopback module then allocate buffers itself (vidioc_reqbufs) and start streaming (vidioc_streamon).
    * data from user-space address space are then copied to v4l2loopback's buffer by copy_from_user() function.
    * This is not efficient because of context switches of write() system calls and overhead of validation check when copying data from user-space to kernel-space with copy_from_user(). It is also not flexible for controling the allocated buffers. 
* Memory Mapping method
    * v4l2 supports Memory Mapping method; it can be confirmed via the V4L2_CAP_STREAMING flag in the capabilities field of struct v4l2_capability returned by the ioctl VIDIOC_QUERYCAP ioctl is set. 
    * User-space application firstly needs to allocate device buffers by calling the ioctl VIDIOC_REQBUFS ioctl with the desired number of buffers and buffer type, for example V4L2_BUF_TYPE_VIDEO_OUTPUT.
    * Before applications can access the buffers they must map them into their address space with the mmap() function. The location of the buffers in device memory can be determined with the ioctl VIDIOC_QUERYBUF ioctl. Applications should free the buffers as soon as possible with the munmap() function.
    * Data from user-space address space are written directly to mmap area, application then uses ioctl for VIDIOC_QBUF/VIDIOC_DQBUF for queuing/dequeuing buffers manually.
    * This is very efficient because it does not use many system calls as write() function (zero-copy overhead). It is also flexible to control the allocated buffers manually. The ioctl can also be used to change the number of buffers or to free the allocated memory, provided none of the buffers are still mapped.

## Artifacts

* v4l2loopback.c
* lbn11-v4l2loopback-write.c
* lbn11-v4l2loopback-write.png (perf report showing overhead of write() system calls and copy_from_user())
* lbn11-v4l2loopback-mmap-pthread.c
* lbn11-v4l2loopback-mmap-pthread.png (perf report showing efficient of Memory Mapping method)
* v4l2.xlsx (reference note)
