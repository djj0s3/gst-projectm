#!/bin/bash
set -e

# If explicitly asked to start the HTTP server (argument or env), do so.
if [[ "$RUNPOD_START_SERVER" == "1" ]]; then
    exec python /app/runpod_pod_server.py "$@"
fi

if [[ "$1" == "serve" ]]; then
    shift
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

# Default: run the RunPod serverless handler (expected in serverless deployments).
exec python /app/runpod_handler.py
