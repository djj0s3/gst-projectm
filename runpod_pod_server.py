import os
import shutil
import subprocess
import tempfile
import traceback
import logging
from pathlib import Path
from typing import Optional

import requests
from fastapi import FastAPI, UploadFile, File, Form, Header, HTTPException, Request, BackgroundTasks
from fastapi.responses import FileResponse, JSONResponse, Response
import uvicorn

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)
logger.info("runpod_pod_server.py: Module loading started")

DEFAULT_PRESET_DIR = os.environ.get("RUNPOD_PRESET_DIR", "/usr/local/share/projectM/presets")
DEFAULT_TEXTURE_DIR = os.environ.get("RUNPOD_TEXTURE_DIR", "/usr/local/share/projectM/textures")
DEFAULT_OUTPUT_NAME = os.environ.get("RUNPOD_OUTPUT_NAME", "output.mp4")
DEFAULT_TIMEOUT_SEC = float(os.environ.get("RUNPOD_CONVERT_TIMEOUT", "10800"))
AUTH_TOKEN = os.environ.get("RUNPOD_POD_AUTH_TOKEN", "").strip()

app = FastAPI(title="ProjectM Renderer Pod")


@app.exception_handler(Exception)
async def global_exception_handler(request: Request, exc: Exception):
    """Catch all unhandled exceptions and return JSON with details."""
    logger.error(f"Unhandled exception: {exc}", exc_info=True)
    return JSONResponse(
        status_code=500,
        content={
            "error": str(exc),
            "type": type(exc).__name__,
            "traceback": traceback.format_exc()[-2000:],
        }
    )


@app.get("/debug")
async def debug():
    """Return diagnostic information about the pod environment."""
    import subprocess
    import glob
    info = {
        "env": {
            "USE_NVIDIA_GPU": os.environ.get("USE_NVIDIA_GPU", "not set"),
            "RUNPOD_START_SERVER": os.environ.get("RUNPOD_START_SERVER", "not set"),
            "DISPLAY": os.environ.get("DISPLAY", "not set"),
            "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH", "not set"),
        },
        "checks": {}
    }

    # Check nvidia-smi
    try:
        result = subprocess.run(["nvidia-smi", "-L"], capture_output=True, text=True, timeout=10)
        info["checks"]["nvidia_smi"] = result.stdout.strip() if result.returncode == 0 else result.stderr.strip()
    except Exception as e:
        info["checks"]["nvidia_smi"] = f"Error: {e}"

    # Check /dev/dri
    try:
        result = subprocess.run(["ls", "-la", "/dev/dri/"], capture_output=True, text=True, timeout=5)
        info["checks"]["dev_dri"] = result.stdout.strip() if result.returncode == 0 else result.stderr.strip()
    except Exception as e:
        info["checks"]["dev_dri"] = f"Error: {e}"

    # Check NVIDIA driver version
    try:
        result = subprocess.run(["cat", "/proc/driver/nvidia/version"], capture_output=True, text=True, timeout=5)
        info["checks"]["nvidia_driver_version"] = result.stdout.strip()[:200] if result.returncode == 0 else "not found"
    except Exception as e:
        info["checks"]["nvidia_driver_version"] = f"Error: {e}"

    # Check for NVIDIA libraries
    nvidia_libs = []
    for pattern in ["/usr/lib/x86_64-linux-gnu/libnvidia*", "/usr/lib/libnvidia*", "/usr/local/nvidia/lib64/*"]:
        nvidia_libs.extend(glob.glob(pattern))
    info["checks"]["nvidia_libs_count"] = len(nvidia_libs)
    info["checks"]["nvidia_libs_sample"] = nvidia_libs[:10] if nvidia_libs else "none found"

    # Check for X driver modules
    xorg_drivers = []
    for pattern in ["/usr/lib/xorg/modules/drivers/*", "/usr/lib/x86_64-linux-gnu/xorg/extra-modules/*"]:
        xorg_drivers.extend(glob.glob(pattern))
    info["checks"]["xorg_drivers"] = [os.path.basename(d) for d in xorg_drivers]

    # Check EGL vendors
    egl_vendors = glob.glob("/usr/share/glvnd/egl_vendor.d/*.json")
    info["checks"]["egl_vendors"] = [os.path.basename(v) for v in egl_vendors]

    # Check libGL implementation
    try:
        result = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True, timeout=5)
        libgl_lines = [l.strip() for l in result.stdout.split('\n') if 'libGL' in l or 'libEGL' in l]
        info["checks"]["libgl_ldconfig"] = libgl_lines[:5]
    except Exception as e:
        info["checks"]["libgl_ldconfig"] = f"Error: {e}"

    # Check glxinfo if available
    try:
        result = subprocess.run(["glxinfo", "-B"], capture_output=True, text=True, timeout=10,
                               env={**os.environ, "DISPLAY": ":0"})
        info["checks"]["glxinfo"] = result.stdout[:500] if result.returncode == 0 else result.stderr[:200]
    except Exception as e:
        info["checks"]["glxinfo"] = f"Error: {e}"

    # Check xorg configs
    info["checks"]["xorg_conf_exists"] = os.path.exists("/etc/X11/xorg.conf")
    info["checks"]["xorg_nvidia_conf_exists"] = os.path.exists("/etc/X11/xorg-nvidia.conf")

    return info


def _require_auth(header: Optional[str]) -> None:
    if AUTH_TOKEN:
        if not header or not header.startswith("Bearer "):
            raise HTTPException(status_code=401, detail="Missing bearer token")
        token = header.split(" ", 1)[1]
        if token != AUTH_TOKEN:
            raise HTTPException(status_code=401, detail="Invalid bearer token")


def _download_to(path: Path, url: str) -> None:
    with requests.get(url, stream=True, timeout=120) as resp:
        resp.raise_for_status()
        with path.open('wb') as dst:
            for chunk in resp.iter_content(chunk_size=1024 * 1024):
                if chunk:
                    dst.write(chunk)


@app.post("/render")
async def render(
    authorization: Optional[str] = Header(None),
    audio_file: Optional[UploadFile] = File(None),
    audio_url: Optional[str] = Form(None),
    timeline_file: Optional[UploadFile] = File(None),
    timeline_url: Optional[str] = Form(None),
    timeline_ini: Optional[str] = Form(None),
    video_width: int = Form(1920),
    video_height: int = Form(1080),
    fps: int = Form(60),
    bitrate_kbps: int = Form(8000),
    mesh: str = Form(os.environ.get("PROJECTM_MESH", "320x240")),
    encoder_speed: str = Form(os.environ.get("PROJECTM_ENCODER_SPEED", "veryfast")),
    preset_duration: int = Form(60),
    timeout_sec: float = Form(DEFAULT_TIMEOUT_SEC),
):
    _require_auth(authorization)

    if audio_file is None and not audio_url:
        raise HTTPException(status_code=400, detail="Must supply audio_file or audio_url")

    with tempfile.TemporaryDirectory(prefix="render_pod_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        audio_path = tmp_path / (audio_file.filename if audio_file else "input_audio")
        if audio_url:
            try:
                _download_to(audio_path, audio_url)
            except Exception as exc:  # noqa: BLE001
                raise HTTPException(status_code=400, detail=f"Failed to download audio: {exc}") from exc
        else:
            with audio_path.open('wb') as dst:
                shutil.copyfileobj(audio_file.file, dst)

        timeline_path = None
        if timeline_url:
            timeline_path = tmp_path / "timeline.ini"
            try:
                _download_to(timeline_path, timeline_url)
            except Exception as exc:  # noqa: BLE001
                raise HTTPException(status_code=400, detail=f"Failed to download timeline: {exc}") from exc
        elif timeline_file:
            timeline_path = tmp_path / (timeline_file.filename or "timeline.ini")
            with timeline_path.open('wb') as dst:
                shutil.copyfileobj(timeline_file.file, dst)
        elif timeline_ini:
            timeline_path = tmp_path / "timeline.ini"
            timeline_path.write_text(timeline_ini, encoding="utf-8")

        output_path = tmp_path / DEFAULT_OUTPUT_NAME

        cmd = [
            "/app/convert.sh",
            "-i",
            str(audio_path),
            "-o",
            str(output_path),
            "-p",
            DEFAULT_PRESET_DIR,
            "--texture",
            DEFAULT_TEXTURE_DIR,
            "--mesh",
            mesh,
            "--video-size",
            f"{video_width}x{video_height}",
            "-r",
            str(fps),
            "-b",
            str(bitrate_kbps),
            "--speed",
            encoder_speed,
        ]

        if timeline_path is not None:
            cmd.extend(["--timeline", str(timeline_path)])
        else:
            cmd.extend(["-d", str(preset_duration)])

        try:
            completed = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout_sec,
            )
        except subprocess.TimeoutExpired as e:
            raise HTTPException(status_code=504, detail={
                "error": "Conversion timed out",
                "stdout": getattr(e, 'stdout', '')[-2000:] if getattr(e, 'stdout', None) else '',
                "stderr": getattr(e, 'stderr', '')[-2000:] if getattr(e, 'stderr', None) else '',
            })
        except Exception as e:
            raise HTTPException(status_code=500, detail={
                "error": f"Subprocess error: {type(e).__name__}: {str(e)}",
                "cmd": cmd,
            })

        # Log subprocess output for debugging
        logger.info(f"Subprocess return code: {completed.returncode}")
        logger.info(f"Output path: {output_path}")
        logger.info(f"Output file exists: {output_path.exists()}")
        if completed.stdout:
            logger.info(f"stdout (last 1000 chars): {completed.stdout[-1000:]}")
        if completed.stderr:
            logger.info(f"stderr (last 1000 chars): {completed.stderr[-1000:]}")

        # List tmp directory contents for debugging
        try:
            contents = list(tmp_path.iterdir())
            logger.info(f"Temp directory contents: {contents}")
        except Exception as e:
            logger.error(f"Could not list temp directory: {e}")

        if completed.returncode != 0 or not output_path.exists():
            detail = {
                "returncode": completed.returncode,
                "output_exists": output_path.exists(),
                "stdout": completed.stdout[-2000:] if completed.stdout else "",
                "stderr": completed.stderr[-2000:] if completed.stderr else "",
            }
            raise HTTPException(status_code=500, detail=detail)

        # Read file into memory before temp directory is cleaned up
        # (FileResponse is async and temp dir gets deleted before it can stream)
        with open(output_path, "rb") as f:
            video_data = f.read()

        headers = {
            "Content-Disposition": f'attachment; filename="{DEFAULT_OUTPUT_NAME}"',
        }
        return Response(
            content=video_data,
            media_type="video/mp4",
            headers=headers,
        )


if __name__ == "__main__":
    port = int(os.environ.get("RUNPOD_POD_PORT", "8000"))
    logger.info(f"Starting server on 0.0.0.0:{port}")
    uvicorn.run(app, host="0.0.0.0", port=port)
