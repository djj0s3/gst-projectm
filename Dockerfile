# Start from NVIDIA CUDA base image for NVENC hardware encoding support
FROM nvidia/cuda:12.2.0-devel-ubuntu22.04

# Install required packages
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        llvm \
        git \
        cmake \
        ca-certificates \
        libssl-dev \
        curl \
        wget \
        pkg-config \
        ninja-build \
        meson \
        flex \
        bison \
        nasm \
        xvfb \
        xserver-xorg-core \
        xserver-xorg-video-dummy \
        x11-xserver-utils \
        kmod \
        libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-ugly \
        gstreamer1.0-gl \
        gstreamer1.0-x \
        gstreamer1.0-tools \
        libgles2-mesa-dev \
        libegl1 \
        libegl-dev \
        libglvnd0 \
        libglvnd-dev \
        libgbm1 \
        libgbm-dev \
        libdrm2 \
        libdrm-dev \
        libgudev-1.0-dev \
        mesa-utils \
        python3 \
        python3-pip \
        python3-venv \
        sudo \
        openssh-server && \
    # NOTE: Do NOT install libnvidia-encode or libnvidia-decode here!
    # These get baked in with a specific driver version (e.g., 525) which conflicts
    # with the host's driver (e.g., 570) and causes NVENC to fail.
    # nvidia-container-toolkit will inject the correct matching libraries at runtime
    # when NVIDIA_DRIVER_CAPABILITIES=video is set.
    #
    # Install NVIDIA OpenGL libraries for ProjectM rendering (EGL/GLX support)
    # Try multiple versions in case specific version isn't available
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libnvidia-gl-535 2>/dev/null || \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libnvidia-gl-525 2>/dev/null || \
    echo "Note: NVIDIA GL libraries not installed (will rely on nvidia-container-toolkit)"

# Create NVIDIA EGL vendor file for proper GPU detection
# This tells libglvnd to use NVIDIA's EGL implementation
RUN mkdir -p /usr/share/glvnd/egl_vendor.d && \
    echo '{"file_format_version": "1.0.0", "ICD": {"library_path": "libEGL_nvidia.so.0"}}' \
    > /usr/share/glvnd/egl_vendor.d/10_nvidia.json

# Download NVIDIA Video Codec SDK headers for NVENC/NVDEC
RUN git clone --depth 1 https://git.videolan.org/git/ffmpeg/nv-codec-headers.git /tmp/nv-codec-headers && \
    cd /tmp/nv-codec-headers && \
    make install && \
    rm -rf /tmp/nv-codec-headers

# Install GStreamer plugins including the bad plugins with NVENC support
# Note: gstreamer1.0-plugins-bad includes nvcodec when CUDA is available
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        gstreamer1.0-plugins-bad \
        libgstreamer-plugins-bad1.0-dev \
        gstreamer1.0-plugins-base-apps

# Build just the nvcodec GStreamer plugin from gstreamer monorepo
# This ensures we have nvh264enc for hardware encoding
RUN pip3 install --upgrade meson && \
    git clone --depth 1 --branch 1.20.7 https://gitlab.freedesktop.org/gstreamer/gstreamer.git /tmp/gstreamer && \
    cd /tmp/gstreamer/subprojects/gst-plugins-bad && \
    meson setup builddir \
        --prefix=/usr \
        --buildtype=release \
        -Dauto_features=disabled \
        -Dnvcodec=enabled \
        -Dgpl=enabled && \
    ninja -C builddir && \
    mkdir -p /usr/lib/x86_64-linux-gnu/gstreamer-1.0 && \
    cp builddir/sys/nvcodec/libgstnvcodec.so /usr/lib/x86_64-linux-gnu/gstreamer-1.0/ 2>/dev/null || \
    find builddir -name "libgstnvcodec.so" -exec cp {} /usr/lib/x86_64-linux-gnu/gstreamer-1.0/ \; && \
    rm -rf /tmp/gstreamer

# Clone the projectM repository and build it
RUN git clone --depth 1 https://github.com/projectM-visualizer/projectm.git /tmp/projectm
WORKDIR /tmp/projectm
RUN git submodule update --init --depth 1
RUN mkdir build
WORKDIR /tmp/projectm/build
RUN cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local ..
RUN make -j$(nproc)
RUN make install
WORKDIR /tmp
RUN rm -rf /tmp/projectm

# Copy preset and texture packs from build context
COPY presets /usr/local/share/projectM/presets
COPY textures /usr/local/share/projectM/textures

# Copy VJ Studio logo for "Made With" overlay
COPY vj_studio_logo.png /app/vj_studio_logo.png

# Install Python dependencies for RunPod serverless handler support (PEP 668 compliant)
RUN python3 -m venv /opt/runpod-env
ENV VIRTUAL_ENV=/opt/runpod-env
ENV PATH="$VIRTUAL_ENV/bin:$PATH"
RUN pip install --no-cache-dir runpod requests fastapi uvicorn boto3

# Copy the local gst-projectm source and build the GStreamer plugin
COPY . /tmp/gst-projectm
# Trim the git history to avoid bloating the image layer / exhausting disk
RUN rm -rf /tmp/gst-projectm/.git
WORKDIR /tmp/gst-projectm
RUN ./setup.sh --auto
RUN rm -rf build && \
    mkdir build && \
    cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make
RUN mkdir -p $(pkg-config --variable=pluginsdir gstreamer-1.0) && \
    cp build/libgstprojectm.so $(pkg-config --variable=pluginsdir gstreamer-1.0)/ && \
    rm -rf /tmp/gst-projectm

# Clean up unnecessary packages to reduce image size
RUN apt-get remove -y \
        build-essential \
        git \
        cmake \
        meson \
        ninja-build \
        flex \
        bison \
        nasm && \
    apt-get autoremove -y && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Create a working directory for conversion tasks
WORKDIR /app

# Copy the conversion script
COPY convert.sh /app/
RUN chmod +x /app/convert.sh

# Copy RunPod serverless handler stub (used when deploying as a serverless endpoint)
COPY runpod_handler.py /app/runpod_handler.py
COPY runpod_pod_server.py /app/runpod_pod_server.py
COPY start.sh /app/start.sh
RUN chmod +x /app/start.sh

# Set environment variables
ENV GST_DEBUG=3
ENV PRESETS_DIR=/usr/local/share/projectM/presets
ENV TEXTURES_DIR=/usr/local/share/projectM/textures
ENV XDG_RUNTIME_DIR=/tmp

# Ensure GStreamer can find plugins (including NVENC from custom build)
ENV GST_PLUGIN_PATH=/usr/lib/x86_64-linux-gnu/gstreamer-1.0:/usr/local/lib/gstreamer-1.0:/usr/lib/gstreamer-1.0
ENV GST_PLUGIN_SCANNER=/usr/lib/x86_64-linux-gnu/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner

# Setup for GPU access - respect Runpod's settings
ENV LIBGL_ALWAYS_INDIRECT=0
# Include 'video' capability for NVENC hardware encoding
ENV NVIDIA_DRIVER_CAPABILITIES=${NVIDIA_DRIVER_CAPABILITIES:-compute,utility,graphics,video}
# Don't override NVIDIA_VISIBLE_DEVICES if already set by Runpod

# OpenGL debugging and shader compatibility
# ProjectM needs at least OpenGL 3.3, use 4.5 for better compatibility
ENV MESA_GL_VERSION_OVERRIDE=4.5
ENV MESA_GLSL_VERSION_OVERRIDE=450
ENV GST_GL_SHADER_DEBUG=0

# Copy xorg configs for both rendering modes
# xorg.conf: dummy driver + Mesa software rendering (v35 stable fallback)
# xorg-nvidia.conf: NVIDIA driver for GPU-accelerated rendering (v36-gpu)
COPY xorg.conf /etc/X11/xorg.conf
COPY xorg-nvidia.conf /etc/X11/xorg-nvidia.conf

# Ensure libraries are found
ENV LD_LIBRARY_PATH=/usr/local/lib:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH}

# Configure SSH for RunPod pod access
RUN mkdir -p /var/run/sshd && \
    ssh-keygen -A && \
    echo 'root:runpod' | chpasswd && \
    sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config && \
    sed -i 's/#PasswordAuthentication yes/PasswordAuthentication yes/' /etc/ssh/sshd_config && \
    sed -i 's/#Port 22/Port 22/' /etc/ssh/sshd_config && \
    echo "export PATH=$PATH" >> /root/.bashrc && \
    echo "export LD_LIBRARY_PATH=$LD_LIBRARY_PATH" >> /root/.bashrc

# Expose SSH and HTTP ports
EXPOSE 22 8000

# Default entrypoint dispatches between conversion and server modes
ENTRYPOINT ["/app/start.sh"]
