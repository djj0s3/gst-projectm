import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Optional

import requests
from fastapi import FastAPI, UploadFile, File, Form, Header, HTTPException
from fastapi.responses import FileResponse
import uvicorn

DEFAULT_PRESET_DIR = os.environ.get("RUNPOD_PRESET_DIR", "/usr/local/share/projectM/presets")
DEFAULT_TEXTURE_DIR = os.environ.get("RUNPOD_TEXTURE_DIR", "/usr/local/share/projectM/textures")
DEFAULT_OUTPUT_NAME = os.environ.get("RUNPOD_OUTPUT_NAME", "output.mp4")
DEFAULT_TIMEOUT_SEC = float(os.environ.get("RUNPOD_CONVERT_TIMEOUT", "10800"))
AUTH_TOKEN = os.environ.get("RUNPOD_POD_AUTH_TOKEN", "").strip()

app = FastAPI(title="ProjectM Renderer Pod")


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
        except subprocess.TimeoutExpired:
            raise HTTPException(status_code=504, detail="Conversion timed out")

        if completed.returncode != 0 or not output_path.exists():
            detail = {
                "stdout": completed.stdout[-2000:],
                "stderr": completed.stderr[-2000:],
            }
            raise HTTPException(status_code=500, detail=detail)

        headers = {
            "X-Convert-Stdout": completed.stdout[-512:],
            "X-Convert-Stderr": completed.stderr[-512:],
        }
        return FileResponse(path=output_path, filename=DEFAULT_OUTPUT_NAME, headers=headers)


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=int(os.environ.get("RUNPOD_POD_PORT", "8000")))
