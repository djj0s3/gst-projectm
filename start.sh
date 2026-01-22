#!/bin/bash

# Startup logging
echo "=== Container Startup ==="
echo "Date: $(date)"
echo "RUNPOD_START_SERVER: ${RUNPOD_START_SERVER:-not set}"
echo "USE_NVIDIA_GPU: ${USE_NVIDIA_GPU:-not set}"
echo "Args: $@"
echo "Hostname: $(hostname)"
echo "Working directory: $(pwd)"
echo "========================="

# Start SSH daemon for RunPod pod access (if not already running)
if [ -x /usr/sbin/sshd ] && ! pgrep -x sshd > /dev/null; then
    echo "Starting SSH daemon..."
    /usr/sbin/sshd
    echo "SSH daemon started on port 22"
fi

# If explicitly asked to start the HTTP server (argument or env), do so.
if [[ "$RUNPOD_START_SERVER" == "1" ]]; then
    echo "Starting pod server on port ${RUNPOD_POD_PORT:-8000}..."
    exec python /app/runpod_pod_server.py "$@"
fi

if [[ "$1" == "serve" ]]; then
    shift
    echo "Starting pod server (serve mode) on port ${RUNPOD_POD_PORT:-8000}..."
    exec python /app/runpod_pod_server.py "$@"
fi

if [[ "$1" == "python" && "$2" == "/app/runpod_pod_server.py" ]]; then
    shift
    exec python "$@"
fi

# If arguments were provided, treat them as convert.sh invocation (legacy/pod mode).
if [[ "$#" -gt 0 ]]; then
    exec /app/convert.sh "$@"
fi

# Check if we're in a serverless environment
if [[ -n "$RUNPOD_ENDPOINT_ID" || -n "$RUNPOD_JOB_ID" ]]; then
    # Serverless mode - use the serverless handler
    echo "Starting serverless handler..."
    exec python /app/runpod_handler.py
else
    # Pod mode without server flag - start the HTTP server by default
    echo "Starting pod server (default mode) on port ${RUNPOD_POD_PORT:-8000}..."
    exec python /app/runpod_pod_server.py
fi
