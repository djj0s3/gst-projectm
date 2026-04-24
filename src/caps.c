
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/audio/audio-format.h>
#include <gst/video/video-format.h>

#include "caps.h"
#include "plugin.h"

GST_DEBUG_CATEGORY_STATIC(gst_projectm_caps_debug);
#define GST_CAT_DEFAULT gst_projectm_caps_debug

const gchar *get_audio_sink_cap(unsigned int type) {
  const char *format;

  switch (type) {
  case 0:
    format =
        GST_AUDIO_CAPS_MAKE("audio/x-raw, "
                            "format = (string) " GST_AUDIO_NE(
                                S16) ", "
                                     "layout = (string) interleaved, "
                                     "channels = (int) { 2 }, "
                                     "rate = (int) { 44100 }, "
                                     "channel-mask = (bitmask) { 0x0003 }");
    break;
  default:
    format = NULL;
    break;
  }

  return format;
}

const gchar *get_video_src_cap(unsigned int type) {
  const char *format;

  switch (type) {
  case 0:
    /* Output formats:
     *   - ABGR: legacy CPU readback path (renders RGBA to FBO, ReadPixels to
     *     CPU, downstream videoconvert handles further conversion). Slow on
     *     macOS because videoconvert ABGR→NV12 dominates render time.
     *   - NV12: GPU-converted output. Plugin runs RGBA→NV12 shader passes
     *     against the FBO and ReadPixels each plane. Eliminates downstream
     *     videoconvert entirely on the path projectm → vtenc_h264.
     *
     * Caps negotiation will pick whichever the downstream prefers; vtenc_h264
     * advertises NV12 in its sink caps so it gets chosen automatically when
     * vtenc is downstream. ABGR remains the fallback for any consumer that
     * doesn't accept NV12.
     */
    format = GST_VIDEO_CAPS_MAKE("video/x-raw, format = (string) { ABGR, NV12 }, "
                                 "framerate=(fraction)[0/1,MAX]");
    break;
  default:
    format = NULL;
    break;
  }

  return format;
}