## lowlatency-live
A plain-C example that shows one example gstreamer pipeline for low-latency RTSP
video receiving and decoding. 

It is based on these tutorials, which you may want to read for better
insights:

 *   [GStreamer basic tutorial 5](https://gstreamer.freedesktop.org/documentation/tutorials/basic/toolkit-integration.html?gi-language=c)
 *   [GStreamer basic tutorial 12](https://gstreamer.freedesktop.org/documentation/tutorials/basic/streaming.html?gi-language=c)
 *   [This documentation on Overlay](https://valadoc.org/gstreamer-video-1.0/Gst.Video.Overlay.html#!)

Only tested on Linux

###Prerequisites

[Install GStreamer](https://gstreamer.freedesktop.org/documentation/installing/on-linux.html?gi-language=c)

###Build

```
gcc demo.c -o demo `pkg-config --cflags --libs gstreamer-video-1.0 gtk+-3.0 gstreamer-1.0
```

