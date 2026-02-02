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

# Process IDs for the gst-launch process and X servers
GST_PID=""
XVFB_PID=""
XORG_PID=""

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

start_x_with_gpu() {
    # Use GLX through X server
    export GST_GL_PLATFORM=glx
    export GST_GL_WINDOW=x11
    export GST_GL_API=opengl3
    export GST_GL_CONFIG=rgba

    # Check if an X server is already running (passed from host)
    # This is the preferred mode when running in Docker with host X11 passthrough
    if [ -n "$DISPLAY" ] && [ -S "/tmp/.X11-unix/X${DISPLAY#:}" ]; then
        echo "Using existing X server at DISPLAY=$DISPLAY (host passthrough)"

        # Test if it works with NVIDIA GLX
        if command -v glxinfo >/dev/null 2>&1; then
            GLX_TEST=$(glxinfo 2>&1 | grep -i "OpenGL renderer" | head -1)
            echo "GLX renderer: $GLX_TEST"
            if echo "$GLX_TEST" | grep -qiE "nvidia|amd|intel|radeon" && ! echo "$GLX_TEST" | grep -qi "llvmpipe"; then
                echo "GPU rendering confirmed via host X server"
                RENDER_MODE="Host X11 passthrough (GPU)"
                return 0
            else
                echo "WARNING: Host X server doesn't provide GPU rendering, will try other methods..."
            fi
        else
            # Assume host display works
            echo "Assuming host X server works (glxinfo not available for test)"
            RENDER_MODE="Host X11 passthrough"
            return 0
        fi
    fi

    # No working host X server, start our own
    # Use unique display number based on process ID to avoid conflicts
    DISPLAY_NUM=$((99 + ($$  % 100)))
    X_LOCK_FILE="/tmp/.X${DISPLAY_NUM}-lock"

    # Clean up stale X lock file if present
    if [ -f "$X_LOCK_FILE" ]; then
        echo "Removing stale X lock file..."
        rm -f "$X_LOCK_FILE"
    fi

    export DISPLAY=:${DISPLAY_NUM}

    # Check if we should use NVIDIA GPU rendering (set USE_NVIDIA_GPU=1 for pods)
    if [ "${USE_NVIDIA_GPU:-0}" -eq 1 ]; then
        echo "========================================"
        echo "GPU ENVIRONMENT DIAGNOSTICS"
        echo "========================================"

        # Show nvidia-smi info
        if command -v nvidia-smi >/dev/null 2>&1; then
            echo "NVIDIA SMI available:"
            nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader 2>/dev/null || echo "  (query failed)"
        else
            echo "NVIDIA SMI: not available"
        fi

        # Check for DRI devices
        echo ""
        echo "DRI devices:"
        ls -la /dev/dri/ 2>/dev/null || echo "  /dev/dri not found"

        # Check for NVIDIA devices
        echo ""
        echo "NVIDIA devices:"
        ls -la /dev/nvidia* 2>/dev/null || echo "  /dev/nvidia* not found"

        # Check EGL vendor files
        echo ""
        echo "EGL vendor files:"
        ls -la /usr/share/glvnd/egl_vendor.d/ 2>/dev/null || echo "  EGL vendor dir not found"

        # Check for required libraries
        echo ""
        echo "GPU libraries:"
        ldconfig -p 2>/dev/null | grep -E "(EGL|GLX)_nvidia" | head -5 || echo "  No NVIDIA GL libraries found"

        echo "========================================"
        echo ""
        echo "Attempting GPU-accelerated rendering..."

        # Check for DRI render node
        RENDER_NODE=$(ls /dev/dri/renderD* 2>/dev/null | head -1)
        CARD_NODE=$(ls /dev/dri/card* 2>/dev/null | head -1)
        HAS_NVIDIA_EGL=$(test -f /usr/share/glvnd/egl_vendor.d/10_nvidia.json && echo "yes" || echo "no")
        HAS_NVIDIA_GLX=$(ldconfig -p 2>/dev/null | grep -q "libGLX_nvidia" && echo "yes" || echo "no")

        echo "Render node: ${RENDER_NODE:-none}"
        echo "Card node: ${CARD_NODE:-none}"
        echo "NVIDIA EGL vendor available: $HAS_NVIDIA_EGL"
        echo "NVIDIA GLX available: $HAS_NVIDIA_GLX"

        # Check if DRI render node is accessible (can be opened)
        DRI_ACCESSIBLE="no"
        if [ -n "$RENDER_NODE" ]; then
            if python3 -c "import os; fd=os.open('$RENDER_NODE', os.O_RDWR); os.close(fd)" 2>/dev/null; then
                DRI_ACCESSIBLE="yes"
                echo "DRI render node is accessible"
            else
                echo "DRI render node exists but is NOT accessible (permission denied)"
            fi
        fi

        # Track if we found a working GPU method
        GPU_METHOD_FOUND=0

        # Method 1: Xorg with modesetting driver + NVIDIA GPU (most reliable)
        # Uses DRM/KMS through modesetting driver with glamor acceleration
        # This approach works because nvidia-container-runtime provides DRM access
        if [ "$GPU_METHOD_FOUND" -eq 0 ] && [ "$DRI_ACCESSIBLE" = "yes" ] && [ -n "$CARD_NODE" ]; then
            echo "Trying Xorg modesetting + NVIDIA GPU..."

            # Use the nvidia xorg config with modesetting driver
            XORG_NVIDIA_CONF="/etc/X11/xorg-nvidia.conf"
            if [ ! -f "$XORG_NVIDIA_CONF" ]; then
                XORG_NVIDIA_CONF="/etc/X11/xorg.conf"
            fi

            # Configure for GPU-accelerated GLX
            export GST_GL_PLATFORM=glx
            export GST_GL_WINDOW=x11
            export GST_GL_API=opengl3
            export __GL_SYNC_TO_VBLANK=0
            export vblank_mode=0

            # Start Xorg with modesetting driver
            Xorg :${DISPLAY_NUM} \
                -config "$XORG_NVIDIA_CONF" \
                -noreset \
                +extension GLX \
                +extension RANDR \
                +extension RENDER \
                -nolisten tcp \
                -logfile /tmp/Xorg.${DISPLAY_NUM}.log &
            XORG_PID=$!
            echo "Started Xorg modesetting on display :${DISPLAY_NUM} (PID: $XORG_PID)"

            sleep 3

            if ! kill -0 $XORG_PID 2>/dev/null; then
                echo "WARNING: Xorg modesetting failed to start"
                cat /tmp/Xorg.${DISPLAY_NUM}.log 2>/dev/null | tail -20
                XORG_PID=""
            else
                # Test if GPU rendering works
                if command -v glxinfo >/dev/null 2>&1; then
                    GLX_TEST=$(glxinfo 2>&1 | grep -i "OpenGL renderer" | head -1)
                    echo "GLX renderer: $GLX_TEST"
                    # Only accept real GPU renderers (NVIDIA, AMD, Intel)
                    # Do NOT accept llvmpipe - gst-projectm outputs black frames with software rendering
                    if echo "$GLX_TEST" | grep -qiE "nvidia|amd|intel|radeon" && ! echo "$GLX_TEST" | grep -qi "llvmpipe"; then
                        echo "GPU rendering confirmed: $GLX_TEST"
                        RENDER_MODE="Xorg modesetting (GPU)"
                        GPU_METHOD_FOUND=1
                    else
                        echo "WARNING: llvmpipe/software renderer detected, trying other methods..."
                        kill -TERM $XORG_PID 2>/dev/null || true
                        sleep 1
                        XORG_PID=""
                    fi
                else
                    # Assume it works
                    RENDER_MODE="Xorg modesetting (GPU)"
                    GPU_METHOD_FOUND=1
                fi
            fi
        fi

        # Method 2: Xvfb + NVIDIA GLX (works on Vast.ai with nvidia-drm modeset=N)
        # This is the most reliable method - forces NVIDIA's GLX client library
        # Works even without DRI access because rendering goes through NVIDIA driver via GLX
        if [ "$GPU_METHOD_FOUND" -eq 0 ] && [ "$HAS_NVIDIA_GLX" = "yes" ]; then
            echo "Trying Xvfb + NVIDIA GLX (preferred method for Vast.ai)..."

            # Start Xvfb
            Xvfb :${DISPLAY_NUM} -screen 0 ${VIDEO_WIDTH}x${VIDEO_HEIGHT}x24 +extension GLX +render -nolisten tcp -noreset &
            XVFB_PID=$!
            echo "Started Xvfb on display :${DISPLAY_NUM} (PID: $XVFB_PID)"

            sleep 2
            if ! kill -0 $XVFB_PID 2>/dev/null; then
                echo "WARNING: Xvfb failed to start"
                XVFB_PID=""
            else
                export GST_GL_PLATFORM=glx
                export GST_GL_WINDOW=x11
                export GST_GL_API=opengl3
                export __GLX_VENDOR_LIBRARY_NAME=nvidia
                export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
                export __GL_SYNC_TO_VBLANK=0
                export vblank_mode=0

                if command -v glxinfo >/dev/null 2>&1; then
                    GLX_TEST=$(glxinfo 2>&1 | grep -i "OpenGL renderer" | head -1)
                    if echo "$GLX_TEST" | grep -qi "nvidia"; then
                        echo "NVIDIA GLX confirmed: $GLX_TEST"
                        RENDER_MODE="Xvfb + NVIDIA GLX (GPU)"
                        GPU_METHOD_FOUND=1
                    else
                        echo "WARNING: NVIDIA GLX not working (got: $GLX_TEST)"
                        kill -TERM $XVFB_PID 2>/dev/null || true
                        sleep 1
                        XVFB_PID=""
                        unset __GLX_VENDOR_LIBRARY_NAME
                    fi
                else
                    RENDER_MODE="Xvfb + NVIDIA GLX (GPU)"
                    GPU_METHOD_FOUND=1
                fi
            fi
        fi

        # EGL methods - try these if GLX methods failed
        # Method 3: EGL with NVIDIA device (most likely to work in containers)
        if [ "$GPU_METHOD_FOUND" -eq 0 ] && [ "$HAS_NVIDIA_EGL" = "yes" ]; then
            echo "Trying EGL with NVIDIA GPU device..."

            # First, check if we can enumerate EGL devices
            if python3 -c "
from ctypes import *
try:
    egl = CDLL('libEGL.so.1')
    EGL_PLATFORM_DEVICE_EXT = 0x313F
    EGLint = c_int32
    EGLBoolean = c_uint32

    # Try to get device list
    eglQueryDevicesEXT = egl.eglQueryDevicesEXT
    eglQueryDevicesEXT.argtypes = [EGLint, c_void_p, POINTER(EGLint)]
    eglQueryDevicesEXT.restype = EGLBoolean

    num = EGLint(0)
    result = eglQueryDevicesEXT(0, None, byref(num))
    print(f'EGL devices available: {num.value}')
    exit(0 if num.value > 0 else 1)
except Exception as e:
    print(f'EGL check failed: {e}')
    exit(1)
" 2>/dev/null; then
                echo "EGL devices found, configuring EGL rendering..."

                export GST_GL_PLATFORM=egl
                export GST_GL_API=opengl3
                export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
                export __GL_SYNC_TO_VBLANK=0
                export vblank_mode=0
                # Force ProjectM to use FBOs for offscreen rendering
                export GST_PROJECTM_FORCE_FBO=1
                unset DISPLAY

                # Check NVIDIA driver version for GBM support (495+ required)
                NVIDIA_VERSION=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1)
                echo "NVIDIA driver version: ${NVIDIA_VERSION:-unknown}"

                # Check if nvidia-drm is loaded with modeset
                if [ -f /sys/module/nvidia_drm/parameters/modeset ]; then
                    MODESET=$(cat /sys/module/nvidia_drm/parameters/modeset 2>/dev/null)
                    echo "nvidia-drm modeset: ${MODESET:-N/A}"
                else
                    echo "nvidia-drm modeset: not available (host kernel module)"
                fi

                # Use a test that actually does FBO rendering (like ProjectM does)
                # gleffects_identity does real shader rendering to FBO
                TEST_PIPELINE="videotestsrc num-buffers=10 ! video/x-raw,width=320,height=240 ! glupload ! glcolorconvert ! glshader fragment=\"void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\" ! glcolorconvert ! gldownload ! fakesink"

                # Simpler test if glshader not available
                SIMPLE_TEST="videotestsrc num-buffers=10 ! video/x-raw,width=320,height=240 ! glupload ! gldownload ! fakesink"

                # Try GBM first - it provides a proper renderable surface
                echo "Testing EGL-GBM with FBO rendering..."
                export GST_GL_WINDOW=gbm
                export GBM_DEVICE="$RENDER_NODE"

                # Test with actual GL rendering
                EGL_GBM_OUTPUT=$(timeout 15 gst-launch-1.0 -e $SIMPLE_TEST 2>&1)
                EGL_GBM_EXIT=$?

                # Show GL context info
                echo "$EGL_GBM_OUTPUT" | grep -i "gl.*context\|renderer\|vendor\|EGL" | head -5
                echo "EGL-GBM test exit code: $EGL_GBM_EXIT"

                if [ $EGL_GBM_EXIT -eq 0 ]; then
                    # Verify it's actually using NVIDIA, not Mesa
                    if echo "$EGL_GBM_OUTPUT" | grep -qi "nvidia"; then
                        RENDER_MODE="EGL-GBM (NVIDIA GPU)"
                        GPU_METHOD_FOUND=1
                        echo "✅ EGL-GBM with NVIDIA GPU validated"
                    else
                        echo "⚠️ EGL-GBM works but using Mesa, not NVIDIA"
                        # Still try it - might work
                        RENDER_MODE="EGL-GBM (Mesa)"
                        GPU_METHOD_FOUND=1
                    fi
                else
                    echo "EGL-GBM failed, trying surfaceless..."
                    echo "GBM errors: $(echo "$EGL_GBM_OUTPUT" | grep -i "error\|failed\|cannot" | head -3)"
                    unset GBM_DEVICE

                    # Try surfaceless EGL as fallback
                    export GST_GL_WINDOW=surfaceless
                    export EGL_PLATFORM=device

                    EGL_TEST_OUTPUT=$(timeout 15 gst-launch-1.0 -e $SIMPLE_TEST 2>&1)
                    EGL_TEST_EXIT=$?

                    echo "$EGL_TEST_OUTPUT" | grep -i "gl.*context\|renderer\|vendor\|EGL" | head -5
                    echo "EGL surfaceless test exit code: $EGL_TEST_EXIT"

                    if [ $EGL_TEST_EXIT -eq 0 ]; then
                        if echo "$EGL_TEST_OUTPUT" | grep -qi "nvidia"; then
                            RENDER_MODE="EGL surfaceless (NVIDIA GPU)"
                            GPU_METHOD_FOUND=1
                            echo "✅ EGL surfaceless with NVIDIA GPU validated"
                        else
                            echo "⚠️ EGL surfaceless works but using Mesa"
                            RENDER_MODE="EGL surfaceless (Mesa)"
                            GPU_METHOD_FOUND=1
                        fi
                    else
                        echo "❌ All EGL methods failed"
                        echo "Surfaceless errors: $(echo "$EGL_TEST_OUTPUT" | grep -i "error\|failed\|cannot" | head -3)"
                        unset GST_GL_PLATFORM GST_GL_WINDOW GST_GL_API EGL_PLATFORM GST_PROJECTM_FORCE_FBO
                    fi
                fi
            else
                echo "No EGL devices found, skipping EGL"
            fi
        fi

        # If no GPU method worked, fall back to Mesa software rendering
        # NOTE: This is common on Vast.ai where nvidia-drm modeset=N prevents GPU OpenGL.
        # WARNING: Mesa llvmpipe produces BLACK FRAMES with gst-projectm plugin!
        # However, NVENC hardware encoding (nvh264enc) may still work - it's independent of GL.
        if [ "$GPU_METHOD_FOUND" -eq 0 ]; then
            echo ""
            echo "❌ CRITICAL: No GPU OpenGL method found!"
            echo "   All tested methods:"
            echo "   - Method 1 (Xorg modesetting): ${XORG_PID:+tried}${XORG_PID:-not available}"
            echo "   - Method 2 (Xvfb + NVIDIA GLX): requires nvidia GLX libs + DRI access"
            echo "   - Method 3 (EGL): requires nvidia-drm modeset=1"
            echo ""
            echo "⚠️  WARNING: Mesa llvmpipe produces BLACK FRAMES with gst-projectm!"
            echo "   The output video will likely be all black."
            echo "   To fix: Use a host with nvidia-drm modeset=Y"
            echo "           or ensure NVIDIA GLX libraries are properly configured."
            USE_NVIDIA_GPU=0
            export MESA_FALLBACK=1
        fi
    fi

    # Fallback to Mesa if USE_NVIDIA_GPU is 0 or was reset above
    if [ "${USE_NVIDIA_GPU:-0}" -eq 0 ]; then
        echo "Starting virtual display with Mesa software rendering..."
        export MESA_FALLBACK=1
        RENDER_MODE="Mesa llvmpipe (CPU)"

        # Force Mesa software rendering for consistent behavior
        export LIBGL_ALWAYS_SOFTWARE=1
        export GALLIUM_DRIVER=llvmpipe
        export __GLX_VENDOR_LIBRARY_NAME=mesa
        export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
        export GST_GL_PLATFORM=glx
        export GST_GL_WINDOW=x11
        export GST_GL_API=opengl3

        # Use Xvfb for simpler, more reliable headless rendering
        Xvfb :${DISPLAY_NUM} -screen 0 ${VIDEO_WIDTH}x${VIDEO_HEIGHT}x24 +extension GLX +render -nolisten tcp &
        XVFB_PID=$!
        echo "Started Xvfb on display :${DISPLAY_NUM} (PID: $XVFB_PID)"
        sleep 2

        if ! kill -0 $XVFB_PID 2>/dev/null; then
            echo "ERROR: Xvfb failed to start"
            # Fall back to Xorg with dummy driver
            echo "Trying Xorg with dummy driver..."
            XORG_CONF="/etc/X11/xorg.conf"
            Xorg :${DISPLAY_NUM} \
                -config "$XORG_CONF" \
                -noreset \
                +extension GLX \
                +extension RANDR \
                +extension RENDER \
                -nolisten tcp \
                -logfile /tmp/Xorg.${DISPLAY_NUM}.log &
            XORG_PID=$!
            XVFB_PID=""
            sleep 2
        fi
    fi

    # Only check X server if we started one (not for EGL-GBM or surfaceless)
    if [ -z "$XORG_PID" ] && [ -z "$XVFB_PID" ]; then
        echo "Using ${RENDER_MODE} without X server"
    elif [ -n "$XVFB_PID" ]; then
        echo "Xvfb running on display :${DISPLAY_NUM} (PID: $XVFB_PID)"
    else
        echo "Started X server on display :${DISPLAY_NUM} (PID: $XORG_PID)"

        # Wait for X server to be ready
        sleep 2
        if ! kill -0 $XORG_PID 2>/dev/null; then
            echo "ERROR: X server failed to start (PID $XORG_PID is not running)"
            echo "--- X server log ---"
            cat /tmp/Xorg.${DISPLAY_NUM}.log 2>/dev/null || echo "No log file found"
            echo "--- end X server log ---"
            exit 1
        fi

        echo "X server running on display :${DISPLAY_NUM}"
    fi

    echo "Rendering Mode: ${RENDER_MODE}"

    # Diagnostic: Check what GL renderer is actually being used
    echo "=== GL Renderer Diagnostics ==="
    if command -v glxinfo >/dev/null 2>&1 && [ -n "$DISPLAY" ]; then
        echo "glxinfo output:"
        glxinfo -B 2>&1 | head -20 || echo "  glxinfo failed"
        echo "GLX vendor check:"
        glxinfo 2>&1 | grep -i "vendor\|renderer\|version" | head -10 || true
    else
        echo "glxinfo not available or no DISPLAY set"
    fi
    echo "==============================="
}

start_xvfb_fallback() {
    echo "Starting Xvfb (CPU software rendering fallback)..."

    # Use unique display number based on process ID to avoid conflicts
    DISPLAY_NUM=$((99 + ($$  % 100)))
    X_LOCK_FILE="/tmp/.X${DISPLAY_NUM}-lock"

    # Clean up stale X lock file if present
    if [ -f "$X_LOCK_FILE" ]; then
        echo "Removing stale X lock file..."
        rm -f "$X_LOCK_FILE"
    fi

    export DISPLAY=:${DISPLAY_NUM}

    # Force Mesa software rendering
    export LIBGL_ALWAYS_SOFTWARE=1
    export GALLIUM_DRIVER=llvmpipe
    export LIBGL_DRIVERS_PATH=/usr/lib/x86_64-linux-gnu/dri
    export MESA_GL_VERSION_OVERRIDE=4.5
    export MESA_GLSL_VERSION_OVERRIDE=450
    # Use GLX for software rendering
    export GST_GL_PLATFORM=glx
    export GST_GL_WINDOW=x11
    export GST_GL_API=opengl3
    export GST_GL_CONFIG=rgba

    Xvfb :${DISPLAY_NUM} -screen 0 ${VIDEO_WIDTH}x${VIDEO_HEIGHT}x24 +extension GLX +render -nolisten tcp -noreset &
    XVFB_PID=$!
    echo "Started Xvfb on display :${DISPLAY_NUM} (PID: $XVFB_PID)"

    # Wait for Xvfb to be ready and verify it started successfully
    sleep 2
    if ! kill -0 $XVFB_PID 2>/dev/null; then
        echo "ERROR: Xvfb failed to start (PID $XVFB_PID is not running)"
        exit 1
    fi
    echo "Xvfb is running (CPU-only)"
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
    # Force FBO rendering - essential for headless EGL (no default framebuffer)
    export GST_PROJECTM_FORCE_FBO=1
}

set_auto_mesh() {
    local width=$1
    local height=$2

    # Reduced mesh size for better performance on software/hybrid rendering
    # Higher mesh = more detail but slower rendering
    # Lower mesh = faster rendering, still looks good
    local auto_x=$((width / 24))
    local auto_y=$((height / 24))

    # Lower minimums for faster rendering (48x27 works well)
    if [ $auto_x -lt 48 ]; then
        auto_x=48
    fi
    if [ $auto_y -lt 27 ]; then
        auto_y=27
    fi

    # Lower maximums to prevent excessive mesh on high-res
    if [ $auto_x -gt 96 ]; then
        auto_x=96
    fi
    if [ $auto_y -gt 54 ]; then
        auto_y=54
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

    # Check for NVIDIA hardware encoding first
    # NVENC (nvh264enc) uses dedicated video encoding hardware that's independent of GL rendering.
    # It works even with Mesa software rendering because it creates its own CUDA context.
    # This is the OPTIMAL configuration on Vast.ai where nvidia-drm modeset=N:
    #   - Mesa llvmpipe for OpenGL rendering (CPU-based, reliable)
    #   - nvh264enc for video encoding (GPU NVENC hardware, fast)
    if command -v nvidia-smi >/dev/null 2>&1 && gst_plugin_available nvh264enc; then
        # Quick test that nvh264enc can actually initialize
        # Note: nvh264enc requires minimum resolution 145x49, so we test with 320x240
        if timeout 5 gst-launch-1.0 -e videotestsrc num-buffers=1 ! video/x-raw,width=320,height=240 ! videoconvert ! "video/x-raw,format=NV12" ! queue ! nvh264enc ! fakesink 2>/dev/null; then
            echo "Using nvh264enc (NVIDIA hardware encoding)"
            if [ "${MESA_FALLBACK:-0}" = "1" ]; then
                echo "  Note: Hybrid mode - Mesa GL rendering + NVENC hardware encoding"
            fi
            ENCODER="nvh264"
            return
        else
            echo "nvh264enc available but test failed, trying alternatives..."
        fi
    fi

    # Other hardware encoders (if GPU mode requested)
    if [ "$use_gpu" -eq 1 ]; then
        if gst_plugin_available vaapih264enc; then
            ENCODER="vaapih264"
            return
        fi

        if gst_plugin_available msdkh264enc; then
            ENCODER="qsvh264"
            return
        fi
    fi

    # Fall back to software encoding
    echo "Using x264 (software encoding)"
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

    if [ ! -z "$XORG_PID" ]; then
        kill -TERM $XORG_PID 2>/dev/null || true
        wait $XORG_PID 2>/dev/null || true
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
            MESH_X=$(echo "$MESH_SIZE" | cut -d'x' -f1)
            MESH_Y=$(echo "$MESH_SIZE" | cut -d'x' -f2)
            MESH_CUSTOM=1
            shift 2
            ;;
        --video-size)
            VIDEO_SIZE="$2"
            VIDEO_WIDTH=$(echo "$VIDEO_SIZE" | cut -d'x' -f1)
            VIDEO_HEIGHT=$(echo "$VIDEO_SIZE" | cut -d'x' -f2)
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
has_hw_encoder=0
use_gpu=0

if has_gpu && gpu_accessible; then
    has_hw_encoder=1
    use_gpu=1
    echo "GPU detected and accessible - enabling GPU rendering"
fi

# Select X server based on GPU availability
if [ "$use_gpu" -eq 1 ]; then
    # Use X server with dummy driver + EGL for GPU acceleration
    start_x_with_gpu
else
    # Fallback to Xvfb software rendering when no GPU
    start_xvfb_fallback
fi

# Select encoder based on GPU availability
select_best_encoder

# Add diagnostic logging for debugging Runpod issues
echo "=== Environment Diagnostics ==="
echo "Hostname: $(hostname)"
echo "GPU Detection: use_gpu=$use_gpu"
echo "DISPLAY: ${DISPLAY:-unset}"
echo "NVIDIA_VISIBLE_DEVICES: ${NVIDIA_VISIBLE_DEVICES:-unset}"
echo "GST_PLUGIN_PATH: ${GST_PLUGIN_PATH:-unset}"
echo "LD_LIBRARY_PATH: ${LD_LIBRARY_PATH:-unset}"
echo "GST_PROJECTM_FORCE_FBO: ${GST_PROJECTM_FORCE_FBO:-unset}"
echo "GST_GL_PLATFORM: ${GST_GL_PLATFORM:-unset}"
echo "GST_GL_WINDOW: ${GST_GL_WINDOW:-unset}"

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
if gst-inspect-1.0 projectm >/dev/null 2>&1; then
    echo "  ✓ projectm plugin found"
    gst-inspect-1.0 projectm | grep -A 5 "Pad Templates" || true
else
    echo "  ERROR: projectm plugin not found!"
    exit 1
fi

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
echo "Rendering Mode: ${RENDER_MODE:-Unknown}"
if [ ! -z "$TIMELINE_FILE" ]; then
    echo "Timeline: $TIMELINE_FILE"
    if [ ! -f "$TIMELINE_FILE" ]; then
        echo "  WARNING: Timeline file does not exist!"
    else
        echo "  Timeline file contents (first 20 lines):"
        head -20 "$TIMELINE_FILE" | sed 's/^/    /'

        # Check if preset paths in timeline actually exist
        echo "  Checking preset paths in timeline:"
        grep -E "^preset\s*=" "$TIMELINE_FILE" | head -5 | while read line; do
            preset_path=$(echo "$line" | sed 's/preset\s*=\s*//')
            if [ -f "$preset_path" ]; then
                echo "    ✓ $preset_path"
            else
                echo "    ✗ MISSING: $preset_path"
            fi
        done
    fi
fi

echo "ProjectM configuration:"
echo "  Preset path: $PRESET_PATH"
echo "  Texture dir: $TEXTURE_DIR"
echo "  Mesh size: ${MESH_X}x${MESH_Y}"
if [ ! -z "$TIMELINE_FILE" ]; then
    echo "  Timeline: $TIMELINE_FILE"
    echo "  Preset duration: (controlled by timeline)"
else
    echo "  Preset duration: ${PRESET_DURATION}s (randomized presets)"
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
# Disable easter egg (W logo that appears at startup)
PROJECTM_ARGS+=("easter-egg=0")

echo ""
echo "=== ProjectM Pre-flight Check ==="
echo "PROJECTM_ARGS: ${PROJECTM_ARGS[@]}"
echo "GPU mode (use_gpu): $use_gpu"

# Quick test: render a few frames with ProjectM to verify the FULL pipeline works
# This catches GL issues that glxinfo misses
echo "Testing ProjectM with current GL context..."
TEST_PNG="/tmp/projectm_test_$$.png"
PREFLIGHT_FAILED=0

# Build test pipeline matching actual render pipeline
if [ "$use_gpu" -eq 1 ]; then
    # GPU mode: projectm outputs GL textures, need gldownload
    echo "  Testing GPU pipeline (with gldownload)..."
    TEST_PIPELINE="audiotestsrc num-buffers=30 ! audioconvert ! audio/x-raw,format=S16LE,channels=2,rate=44100 ! projectm preset=$PRESET_PATH mesh-size=32,24 ! gldownload ! videoconvert ! video/x-raw,width=320,height=240 ! pngenc ! filesink location=$TEST_PNG"
else
    # CPU/Mesa mode: projectm outputs raw video
    echo "  Testing CPU/Mesa pipeline..."
    TEST_PIPELINE="audiotestsrc num-buffers=30 ! audioconvert ! audio/x-raw,format=S16LE,channels=2,rate=44100 ! projectm preset=$PRESET_PATH mesh-size=32,24 ! video/x-raw,width=320,height=240,framerate=30/1 ! videoconvert ! pngenc ! filesink location=$TEST_PNG"
fi

if timeout 15 gst-launch-1.0 -e $TEST_PIPELINE 2>&1; then
    if [ -f "$TEST_PNG" ] && [ -s "$TEST_PNG" ]; then
        PNG_SIZE=$(stat -c%s "$TEST_PNG" 2>/dev/null || stat -f%z "$TEST_PNG")
        echo "✓ ProjectM test render succeeded ($PNG_SIZE bytes)"
        # A very small PNG (< 1KB) might indicate all-black output
        if [ "$PNG_SIZE" -lt 1000 ]; then
            echo "⚠️ Warning: Test output is suspiciously small, might be black"
            PREFLIGHT_FAILED=1
        fi
        rm -f "$TEST_PNG"
    else
        echo "⚠️ ProjectM test render produced empty output"
        PREFLIGHT_FAILED=1
    fi
else
    echo "⚠️ ProjectM test render failed or timed out"
    PREFLIGHT_FAILED=1
fi

# If GPU mode test failed, do NOT fall back to Mesa - it produces black frames!
if [ "$PREFLIGHT_FAILED" -eq 1 ] && [ "$use_gpu" -eq 1 ]; then
    echo ""
    echo "❌ GPU preflight test failed!"
    echo ""
    echo "⚠️  IMPORTANT: Mesa/llvmpipe fallback is NOT viable for gst-projectm."
    echo "   The gst-projectm plugin outputs BLACK FRAMES with software rendering."
    echo ""
    echo "   The rendering will proceed but will likely produce black video."
    echo "   To fix this, ensure the Vast.ai/RunPod host has:"
    echo "   1. NVIDIA GLX libraries accessible"
    echo "   2. DRI render node access (/dev/dri/renderD*)"
    echo "   3. Or nvidia-drm modeset=Y for proper EGL support"
    echo ""
    # Don't fall back to Mesa - just warn and continue
    # The NVIDIA GLX method may still work even if preflight failed
    export MESA_FALLBACK=1
fi

echo "==================================="
echo ""

KEY_INT=$((FRAMERATE * 2))
GL_DOWNLOAD_PIPELINE=""
if [ "$use_gpu" -eq 1 ] || [ "$FORCE_GL_DOWNLOAD" -eq 1 ]; then
    # ProjectM outputs GL textures, download to system memory for encoding
    # Skip glcolorconvert with EGL headless as it can't negotiate context
    # Let videoconvert handle any needed format conversion after gldownload
    GL_DOWNLOAD_PIPELINE="gldownload ! "
fi
case "$ENCODER" in
    x264)
        # Force I420 (yuv420p) format for QuickTime compatibility
        # Without this, x264 may encode in High 4:4:4 profile with yuv444p which QuickTime can't play
        ENCODER_PIPELINE="${GL_DOWNLOAD_PIPELINE}videoconvert ! videorate ! video/x-raw,format=I420,framerate=${FRAMERATE}/1,width=${VIDEO_WIDTH},height=${VIDEO_HEIGHT} ! x264enc bitrate=$BITRATE speed-preset=$SPEED_PRESET key-int-max=$KEY_INT threads=0"
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
# ProjectM needs larger buffers to avoid stalls when processing audio for visualization
VIDEO_QUEUE_OPTS="queue max-size-buffers=256 max-size-bytes=0 max-size-time=0"
H264_POST_ENCODE_PIPELINE="h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au"

# Increase GST_DEBUG for ProjectM visualization issues
export GST_DEBUG="${GST_DEBUG:-3},projectm:5"

# Ensure output directory exists
OUTPUT_DIR=$(dirname "$OUTPUT_FILE")
mkdir -p "$OUTPUT_DIR"

echo "Starting GStreamer pipeline..."
echo "Pipeline: filesrc -> decodebin -> audioconvert/audioresample -> tee"
echo "  Branch 1 (audio): -> AAC encoder -> muxer"
echo "  Branch 2 (video): -> ProjectM -> ${ENCODER} encoder -> muxer"

# Run the actual conversion
# Note: ProjectM requires S16LE audio format, not F32LE
# We decode audio once, then split: one branch converts to F32LE for AAC, other stays S16LE for ProjectM
gst-launch-1.0 -e \
  filesrc location="$INPUT_FILE" ! \
    decodebin ! audioconvert ! audioresample ! \
    audio/x-raw,format=S16LE,channels=2,rate=44100 ! \
    tee name=t \
      t. ! $AUDIO_QUEUE_OPTS ! audioconvert ! audio/x-raw,format=F32LE ! avenc_aac bitrate=320000 ! queue ! mux. \
      t. ! $VIDEO_QUEUE_OPTS ! projectm ${PROJECTM_ARGS[@]} ! \
            ${ENCODER_PIPELINE} ! \
            ${H264_POST_ENCODE_PIPELINE} ! queue ! mux. \
    mp4mux name=mux ! filesink location="$OUTPUT_FILE" &

GST_PID=$!

# Wait for the conversion to finish or for signals with timeout
echo "Conversion running. Press Ctrl+C to stop."

# Calculate max duration (audio duration * 4 + 120 seconds buffer)
MAX_DURATION=7200  # 2 hours max fallback

# Wait with timeout monitoring and check output file size
WAIT_COUNT=0
LAST_SIZE=0
STALL_COUNT=0
echo "Monitoring pipeline (PID $GST_PID)..."
while kill -0 $GST_PID 2>/dev/null; do
    sleep 5
    WAIT_COUNT=$((WAIT_COUNT + 5))

    # Check if output file is growing
    if [ -f "$OUTPUT_FILE" ]; then
        CURRENT_SIZE=$(stat -c%s "$OUTPUT_FILE" 2>/dev/null || echo "0")
        if [ "$CURRENT_SIZE" -eq "$LAST_SIZE" ] && [ "$CURRENT_SIZE" -gt 0 ]; then
            STALL_COUNT=$((STALL_COUNT + 5))
            echo "[${WAIT_COUNT}s] Output stalled at ${CURRENT_SIZE} bytes for ${STALL_COUNT}s"
            # If file hasn't grown in 30 seconds, assume pipeline is stuck
            if [ $STALL_COUNT -gt 30 ]; then
                echo "⚠️  Output file hasn't grown in ${STALL_COUNT}s (${CURRENT_SIZE} bytes), sending EOS"
                kill -INT $GST_PID 2>/dev/null || true
                sleep 5
                # If still running after EOS, force terminate
                if kill -0 $GST_PID 2>/dev/null; then
                    echo "⚠️  Pipeline didn't respond to EOS, force terminating"
                    kill -TERM $GST_PID 2>/dev/null || true
                    sleep 2
                    kill -KILL $GST_PID 2>/dev/null || true
                fi
                break
            fi
        else
            if [ $STALL_COUNT -gt 0 ]; then
                echo "[${WAIT_COUNT}s] Output growing: ${CURRENT_SIZE} bytes (was stalled for ${STALL_COUNT}s)"
            fi
            STALL_COUNT=0
        fi
        LAST_SIZE=$CURRENT_SIZE
    else
        echo "[${WAIT_COUNT}s] Waiting for output file to be created..."
    fi

    # Hard timeout
    if [ $WAIT_COUNT -gt $MAX_DURATION ]; then
        echo "❌ Conversion timed out after $MAX_DURATION seconds, terminating"
        kill -TERM $GST_PID 2>/dev/null || true
        sleep 2
        kill -KILL $GST_PID 2>/dev/null || true
        exit 1
    fi
done

echo "Pipeline process exited, checking result..."
# Check if conversion completed successfully
wait $GST_PID
EXIT_CODE=$?

# Clean up X server before exiting
if [ ! -z "$XVFB_PID" ]; then
    kill -TERM $XVFB_PID 2>/dev/null || true
    # Wait for Xvfb to exit (with timeout)
    XVFB_WAIT=0
    while kill -0 $XVFB_PID 2>/dev/null && [ $XVFB_WAIT -lt 10 ]; do
        sleep 0.5
        XVFB_WAIT=$((XVFB_WAIT + 1))
    done
    # Force kill if still running
    if kill -0 $XVFB_PID 2>/dev/null; then
        kill -KILL $XVFB_PID 2>/dev/null || true
        wait $XVFB_PID 2>/dev/null || true
    fi
fi

if [ ! -z "$XORG_PID" ]; then
    kill -TERM $XORG_PID 2>/dev/null || true
    # Wait for Xorg to exit (with timeout)
    XORG_WAIT=0
    while kill -0 $XORG_PID 2>/dev/null && [ $XORG_WAIT -lt 10 ]; do
        sleep 0.5
        XORG_WAIT=$((XORG_WAIT + 1))
    done
    # Force kill if still running
    if kill -0 $XORG_PID 2>/dev/null; then
        kill -KILL $XORG_PID 2>/dev/null || true
        wait $XORG_PID 2>/dev/null || true
    fi
fi

if [ $EXIT_CODE -eq 0 ]; then
    echo "Conversion complete! Output saved to $OUTPUT_FILE"
else
    echo "❌ Conversion failed with exit code $EXIT_CODE"
    exit $EXIT_CODE
fi
