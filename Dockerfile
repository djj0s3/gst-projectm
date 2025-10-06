# Start from the official Ubuntu 24.04 image
FROM ubuntu:24.04

# Install required packages
RUN sed -i 's|http://|https://|g' /etc/apt/sources.list && \
    echo 'Acquire::AllowInsecureRepositories "true";' > /etc/apt/apt.conf.d/99allow-insecure && \
    echo 'Acquire::AllowDowngradeToInsecureRepositories "true";' >> /etc/apt/apt.conf.d/99allow-insecure && \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends --allow-unauthenticated \
        build-essential \
        llvm \
        git \
        cmake \
        ca-certificates \
        libssl-dev \
        curl \
        xvfb \
        libgstreamer-plugins-base1.0-dev \
        libgstreamer-plugins-bad1.0-dev \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly \
        gstreamer1.0-x \
        gstreamer1.0-tools \
        libgles2-mesa-dev \
        mesa-utils \
        python3 \
        python3-pip \
        python3-venv \
        sudo

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

# Install Python dependencies for RunPod serverless handler support (PEP 668 compliant)
RUN python3 -m venv /opt/runpod-env
ENV VIRTUAL_ENV=/opt/runpod-env
ENV PATH="$VIRTUAL_ENV/bin:$PATH"
RUN pip install --no-cache-dir runpod requests fastapi uvicorn

# Copy the local gst-projectm source and build the GStreamer plugin
COPY . /tmp/gst-projectm
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
        cmake && \
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

# Setup for GPU access
ENV LIBGL_ALWAYS_INDIRECT=0
ENV NVIDIA_DRIVER_CAPABILITIES=all
ENV NVIDIA_VISIBLE_DEVICES=all

# OpenGL debugging and shader compatibility
ENV MESA_GL_VERSION_OVERRIDE=3.3
ENV MESA_GLSL_VERSION_OVERRIDE=330
ENV GST_GL_SHADER_DEBUG=0

# Default entrypoint dispatches between conversion and server modes
ENTRYPOINT ["/app/start.sh"]
