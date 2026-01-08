#!/bin/bash
set -e

# Default values
PRESET_PATH="/usr/local/share/projectM/presets"
TEXTURE_DIR="/usr/local/share/projectM/textures"
TIMELINE_FILE="${TIMELINE_FILE:-}"
PRESET_DURATION=60
MESH_X=128
MESH_Y=72
VIDEO_WIDTH=1920
VIDEO_HEIGHT=1080
FRAMERATE=60
BITRATE=8000
SPEED_PRESET="medium"
ENCODER="auto"
FORCE_GPU=0
FORCE_XVFB=0
FORCE_GL_DOWNLOAD=${FORCE_GL_DOWNLOAD:-0}
MESH_CUSTOM=0

# Process IDs for the gst-launch process and Xvfb
GST_PID=""
XVFB_PID=""

has_gpu() {
    if command -v nvidia-smi >/dev/null 2>&1; then
        return 0
    fi

    for dev in /dev/dri/renderD* /dev/dri/card* /dev/nvidia0; do
        if [ -e "$dev" ]; then
            return 0
        fi
    done

    return 1
}

gpu_accessible() {
    # First check if nvidia-smi actually works (GPU is functional)
    if command -v nvidia-smi >/dev/null 2>&1; then
        if nvidia-smi >/dev/null 2>&1; then
            # nvidia-smi works, GPU is accessible
            return 0
        fi
    fi

    # Otherwise check device nodes
    for dev in /dev/dri/renderD* /dev/dri/card* /dev/nvidia0; do
        if [ -e "$dev" ]; then
            if [ -r "$dev" ] || [ -w "$dev" ]; then
                return 0
            fi
        fi
    done

    return 1
}

start_xvfb() {
    echo "Starting Xvfb (software rendering fallback)..."

    # Use unique display number based on process ID to avoid conflicts
    DISPLAY_NUM=$((99 + ($$  % 100)))
    X_LOCK_FILE="/tmp/.X${DISPLAY_NUM}-lock"

    # Clean up stale X lock file if present
    if [ -f "$X_LOCK_FILE" ]; then
        echo "Removing stale X lock file..."
        rm -f "$X_LOCK_FILE"
    fi

    export DISPLAY=:${DISPLAY_NUM}
    export LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-1}
    export GALLIUM_DRIVER=${GALLIUM_DRIVER:-llvmpipe}
    export LIBGL_DRIVERS_PATH=${LIBGL_DRIVERS_PATH:-/usr/lib/x86_64-linux-gnu/dri}
    export MESA_GL_VERSION_OVERRIDE=${MESA_GL_VERSION_OVERRIDE:-4.5}
    export MESA_GLSL_VERSION_OVERRIDE=${MESA_GLSL_VERSION_OVERRIDE:-450}
    # Use GLX instead of X11 for better compatibility with software rendering
    export GST_GL_PLATFORM=${GST_GL_PLATFORM:-glx}
    export GST_GL_WINDOW=${GST_GL_WINDOW:-x11}
    export GST_GL_API=${GST_GL_API:-opengl}
    export GST_GL_CONFIG=${GST_GL_CONFIG:-rgba}
    Xvfb :${DISPLAY_NUM} -screen 0 ${VIDEO_WIDTH}x${VIDEO_HEIGHT}x24 +extension GLX +render -nolisten tcp -noreset &
    XVFB_PID=$!
    echo "Started Xvfb on display :${DISPLAY_NUM} (PID: $XVFB_PID)"

    # Wait for Xvfb to be ready
    sleep 1
}

use_headless_gpu() {
    echo "Using EGL headless GPU rendering (DISPLAY unset)."
    unset DISPLAY
    export GST_GL_PLATFORM=${GST_GL_PLATFORM:-egl}
    export GST_GL_WINDOW=${GST_GL_WINDOW:-surfaceless}
    export GST_GL_API=${GST_GL_API:-opengl3}
    export GST_GL_CONFIG=${GST_GL_CONFIG:-rgba}
    export GST_GL_EGL_PLATFORM=${GST_GL_EGL_PLATFORM:-surfaceless}
    export EGL_PLATFORM=${EGL_PLATFORM:-surfaceless}
}

set_auto_mesh() {
    local width=$1
    local height=$2

    local auto_x=$((width / 16))
    local auto_y=$((height / 16))

    if [ $auto_x -lt 64 ]; then
        auto_x=64
    fi
    if [ $auto_y -lt 36 ]; then
        auto_y=36
    fi

    if [ $auto_x -gt 192 ]; then
        auto_x=192
    fi
    if [ $auto_y -gt 108 ]; then
        auto_y=108
    fi

    MESH_X=$auto_x
    MESH_Y=$auto_y
}

gst_plugin_available() {
    local plugin="$1"
    if ! command -v gst-inspect-1.0 >/dev/null 2>&1; then
        return 1
    fi
    gst-inspect-1.0 "$plugin" >/dev/null 2>&1
}

select_best_encoder() {
    if [ "$ENCODER" != "auto" ]; then
        return
    fi

    if [ "$use_gpu" -eq 1 ]; then
        if command -v nvidia-smi >/dev/null 2>&1 && gst_plugin_available nvh264enc; then
            ENCODER="nvh264"
            return
        fi

        if gst_plugin_available vaapih264enc; then
            ENCODER="vaapih264"
            return
        fi

        if gst_plugin_available msdkh264enc; then
            ENCODER="qsvh264"
            return
        fi
    fi

    ENCODER="x264"
}

# Signal handler for proper termination
cleanup() {
    echo ""
    echo "Caught signal, stopping conversion..."
    if [ ! -z "$GST_PID" ]; then
        kill -INT $GST_PID 2>/dev/null || true
        sleep 1
        # If it's still running, try harder
        kill -TERM $GST_PID 2>/dev/null || true
    fi

    if [ ! -z "$XVFB_PID" ]; then
        kill -TERM $XVFB_PID 2>/dev/null || true
        wait $XVFB_PID 2>/dev/null || true
    fi

    exit 0
}

# Setup signal traps for proper termination
trap cleanup INT TERM

# Display help information
show_help() {
    echo "ProjectM Audio to Video Converter"
    echo ""
    echo "Usage: $0 [options] -i input_file -o output_file"
    echo ""
    echo "Options:"
    echo "  -i, --input FILE       Input audio file path (required)"
    echo "  -o, --output FILE      Output video file path (required)"
    echo "  -p, --preset DIR       Path to projectM preset directory (default: $PRESET_PATH)"
    echo "  -t, --texture DIR      Path to projectM texture directory (default: $TEXTURE_DIR)"
    echo "  -d, --duration SEC     Preset duration in seconds (default: $PRESET_DURATION)"
    echo "  --mesh WxH             Mesh size (default: ${MESH_X}x${MESH_Y})"
    echo "  --video-size WxH       Output video size (default: ${VIDEO_WIDTH}x${VIDEO_HEIGHT})"
    echo "  -r, --framerate FPS    Output video framerate (default: $FRAMERATE)"
    echo "  -b, --bitrate KBPS     Output video bitrate in kbps (default: $BITRATE)"
    echo "  --speed PRESET         x264 encoding speed preset (default: $SPEED_PRESET, only used with --encoder x264)"
    echo "  --timeline FILE        Path to preset timeline (INI)"
    echo "  --encoder NAME         Encoder: auto (default), x264, nvh264, vaapih264, qsvh264"
    echo "  --force-gpu            Force EGL/DRI headless GPU usage (fail if unavailable)"
    echo "  --force-xvfb           Force legacy software rendering via Xvfb"
    echo "                         Options: ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow"
    echo "  --timeline FILE        Optional preset timeline file (.ini)"
    echo "  -h, --help             Display this help message and exit"
    echo ""
    echo "Example:"
    echo "  $0 -i input.mp3 -o output.mp4 --video-size 3840x2160 -r 30"
    echo ""
    exit 1
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    key="$1"
    case $key in
        -i|--input)
            INPUT_FILE="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT_FILE="$2"
            shift 2
            ;;
        -p|--preset)
            PRESET_PATH="$2"
            shift 2
            ;;
        -t|--texture)
            TEXTURE_DIR="$2"
            shift 2
            ;;
        -d|--duration)
            PRESET_DURATION="$2"
            shift 2
            ;;
        --mesh)
            MESH_SIZE="$2"
            MESH_X=$(echo $MESH_SIZE | cut -d'x' -f1)
            MESH_Y=$(echo $MESH_SIZE | cut -d'x' -f2)
            MESH_CUSTOM=1
            shift 2
            ;;
        --video-size)
            VIDEO_SIZE="$2"
            VIDEO_WIDTH=$(echo $VIDEO_SIZE | cut -d'x' -f1)
            VIDEO_HEIGHT=$(echo $VIDEO_SIZE | cut -d'x' -f2)
            shift 2
            ;;
        -r|--framerate)
            FRAMERATE="$2"
            shift 2
            ;;
        -b|--bitrate)
            BITRATE="$2"
            shift 2
            ;;
        --speed)
            SPEED_PRESET="$2"
            shift 2
            ;;
        --timeline)
            TIMELINE_FILE="$2"
            shift 2
            ;;
        --encoder)
            ENCODER="$2"
            shift 2
            ;;
        --force-gpu)
            FORCE_GPU=1
            shift
            ;;
        --force-xvfb)
            FORCE_XVFB=1
            shift
            ;;
        -h|--help)
            show_help
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            ;;
    esac
done

# Check for required arguments
if [ -z "$INPUT_FILE" ] || [ -z "$OUTPUT_FILE" ]; then
    echo "Error: Input and output files are required"
    show_help
fi

if [ $MESH_CUSTOM -eq 0 ]; then
    set_auto_mesh "$VIDEO_WIDTH" "$VIDEO_HEIGHT"
fi

ENCODER=$(echo "$ENCODER" | tr '[:upper:]' '[:lower:]')

if [ "$FORCE_XVFB" -eq 1 ] && [ "$FORCE_GPU" -eq 1 ]; then
    echo "Error: --force-gpu and --force-xvfb cannot be used together"
    exit 1
fi

# Decide rendering backend
# NOTE: ProjectM doesn't work reliably with headless EGL, so always use Xvfb for rendering
# But we still detect GPU for hardware encoding (nvh264enc)
has_hw_encoder=0
if has_gpu && gpu_accessible; then
    has_hw_encoder=1
    echo "GPU detected and accessible - will use for hardware encoding"
fi

# Always use Xvfb for ProjectM rendering (GL context works better with X11 than headless EGL)
use_gpu=0
start_xvfb

# Override use_gpu temporarily for encoder selection, then restore
use_gpu=$has_hw_encoder
select_best_encoder
use_gpu=0

# Add diagnostic logging for debugging Runpod issues
echo "=== Environment Diagnostics ==="
echo "Hostname: $(hostname)"
echo "GPU Detection: use_gpu=$use_gpu"
echo "DISPLAY: ${DISPLAY:-unset}"
echo "NVIDIA_VISIBLE_DEVICES: ${NVIDIA_VISIBLE_DEVICES:-unset}"
echo "GST_PLUGIN_PATH: ${GST_PLUGIN_PATH:-unset}"
echo "LD_LIBRARY_PATH: ${LD_LIBRARY_PATH:-unset}"

# Check for GPU devices
if [ "$use_gpu" -eq 1 ]; then
    echo "GPU devices found:"
    ls -la /dev/nvidia* 2>&1 || echo "  No /dev/nvidia* devices"
    ls -la /dev/dri/* 2>&1 || echo "  No /dev/dri/* devices"
    if command -v nvidia-smi >/dev/null 2>&1; then
        echo "nvidia-smi output:"
        nvidia-smi 2>&1 || echo "  nvidia-smi failed"
    fi
fi

# Check for GStreamer projectm plugin
echo "GStreamer projectm plugin check:"
gst-inspect-1.0 projectm 2>&1 || echo "  ERROR: projectm plugin not found!"

# Check for input/output file accessibility
echo "Input file: $INPUT_FILE ($(stat -c%s "$INPUT_FILE" 2>/dev/null || stat -f%z "$INPUT_FILE" 2>/dev/null || echo "unknown") bytes)"
echo "Output directory: $(dirname "$OUTPUT_FILE") (writable: $(test -w "$(dirname "$OUTPUT_FILE")" && echo "yes" || echo "NO"))"
echo "==================================="

if [ -z "$INSIDE_DOCKER" ]; then
    export INSIDE_DOCKER=1
fi

echo "Converting $INPUT_FILE to $OUTPUT_FILE"
echo "Preset path: $PRESET_PATH"
echo "Preset duration: $PRESET_DURATION seconds"
if [ $MESH_CUSTOM -eq 0 ]; then
    echo "Mesh size: ${MESH_X}x${MESH_Y} (auto)"
else
    echo "Mesh size: ${MESH_X}x${MESH_Y}"
fi
echo "Video size: ${VIDEO_WIDTH}x${VIDEO_HEIGHT}"
echo "Framerate: $FRAMERATE fps"
echo "Bitrate: $BITRATE kbps"
echo "Encoder: $ENCODER"
if [ "$ENCODER" = "x264" ]; then
    echo "x264 speed preset: $SPEED_PRESET"
else
    echo "Encoder preset flag ignored (handled internally by $ENCODER)"
fi
if [ "$use_gpu" -eq 1 ]; then
    echo "Rendering Mode: Headless EGL (GPU)"
else
    echo "Rendering Mode: Xvfb software fallback"
fi
if [ ! -z "$TIMELINE_FILE" ]; then
    echo "Timeline: $TIMELINE_FILE"
fi

if [ "$use_gpu" -eq 0 ]; then
    # Wait a moment for Xvfb to start
    sleep 1
fi

TIMELINE_PROPERTY=""
if [ ! -z "$TIMELINE_FILE" ]; then
    TIMELINE_PROPERTY="timeline-path=$TIMELINE_FILE"
fi

PROJECTM_ARGS=("preset=$PRESET_PATH" "texture-dir=$TEXTURE_DIR")
if [ -n "$TIMELINE_PROPERTY" ]; then
    PROJECTM_ARGS+=("$TIMELINE_PROPERTY")
else
    PROJECTM_ARGS+=("preset-duration=$PRESET_DURATION")
fi
PROJECTM_ARGS+=("mesh-size=${MESH_X},${MESH_Y}")

KEY_INT=$((FRAMERATE * 2))
GL_DOWNLOAD_PIPELINE=""
if [ "$use_gpu" -eq 1 ] || [ "$FORCE_GL_DOWNLOAD" -eq 1 ]; then
    # ProjectM outputs ABGR in GL memory, download to system memory without format caps
    # Let glcolorconvert and videoconvert handle format conversion automatically
    GL_DOWNLOAD_PIPELINE="glcolorconvert ! gldownload ! "
fi
case "$ENCODER" in
    x264)
        ENCODER_PIPELINE="${GL_DOWNLOAD_PIPELINE}videoconvert ! videorate ! video/x-raw,framerate=${FRAMERATE}/1,width=${VIDEO_WIDTH},height=${VIDEO_HEIGHT} ! x264enc bitrate=$BITRATE speed-preset=$SPEED_PRESET key-int-max=$KEY_INT threads=0"
        ;;
nvh264)
        ENCODER_PIPELINE="${GL_DOWNLOAD_PIPELINE}videoconvert ! videorate ! video/x-raw,format=NV12,framerate=${FRAMERATE}/1,width=${VIDEO_WIDTH},height=${VIDEO_HEIGHT} ! queue ! nvh264enc bitrate=$BITRATE preset=hp rc-mode=cbr-hq gop-size=$KEY_INT"
        ;;
    vaapih264)
        VAAPI_BITRATE=$BITRATE
        ENCODER_PIPELINE="${GL_DOWNLOAD_PIPELINE}videoconvert ! videorate ! video/x-raw,format=NV12,framerate=${FRAMERATE}/1,width=${VIDEO_WIDTH},height=${VIDEO_HEIGHT} ! queue ! vaapih264enc bitrate=$VAAPI_BITRATE keyframe-period=$KEY_INT"
        ;;
    qsvh264)
        QSV_BITRATE=$BITRATE
        ENCODER_PIPELINE="${GL_DOWNLOAD_PIPELINE}videoconvert ! videorate ! video/x-raw,format=NV12,framerate=${FRAMERATE}/1,width=${VIDEO_WIDTH},height=${VIDEO_HEIGHT} ! queue ! msdkh264enc bitrate=$QSV_BITRATE rate-control=cbr gop-size=$KEY_INT"
        ;;
    *)
        echo "Unsupported encoder '$ENCODER'. Supported encoders: x264, nvh264, vaapih264, qsvh264"
        exit 1
        ;;
esac

AUDIO_QUEUE_OPTS="queue max-size-buffers=2048 max-size-bytes=0 max-size-time=0"
VIDEO_QUEUE_OPTS="queue max-size-buffers=12 max-size-bytes=0 max-size-time=0 leaky=downstream"
H264_POST_ENCODE_PIPELINE="h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au"

# Run the actual conversion
gst-launch-1.0 -e \
  filesrc location=$INPUT_FILE ! \
    decodebin ! tee name=t \
      t. ! $AUDIO_QUEUE_OPTS ! audioconvert ! audioresample ! \
            capsfilter caps="audio/x-raw, format=F32LE, channels=2, rate=44100" ! \
            avenc_aac bitrate=320000 ! queue ! mux. \
      t. ! $VIDEO_QUEUE_OPTS ! audioconvert ! projectm \
            ${PROJECTM_ARGS[@]} ! \
            ${ENCODER_PIPELINE} ! \
            ${H264_POST_ENCODE_PIPELINE} ! queue ! mux. \
    mp4mux name=mux ! filesink location=$OUTPUT_FILE &

GST_PID=$!

# Wait for the conversion to finish or for signals with timeout
echo "Conversion running. Press Ctrl+C to stop."

# Calculate max duration (audio duration * 4 + 120 seconds buffer)
MAX_DURATION=7200  # 2 hours max fallback

# Wait with timeout monitoring
WAIT_COUNT=0
while kill -0 $GST_PID 2>/dev/null; do
    sleep 5
    WAIT_COUNT=$((WAIT_COUNT + 5))

    # Hard timeout
    if [ $WAIT_COUNT -gt $MAX_DURATION ]; then
        echo "❌ Conversion timed out after $MAX_DURATION seconds, terminating"
        kill -TERM $GST_PID 2>/dev/null || true
        sleep 2
        kill -KILL $GST_PID 2>/dev/null || true
        exit 1
    fi
done

# Check if conversion completed successfully
wait $GST_PID
EXIT_CODE=$?

if [ $EXIT_CODE -eq 0 ]; then
    echo "Conversion complete! Output saved to $OUTPUT_FILE"
else
    echo "❌ Conversion failed with exit code $EXIT_CODE"
    exit $EXIT_CODE
fi
