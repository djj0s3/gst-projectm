import base64
import json
import os
import subprocess
import tempfile
from pathlib import Path

import runpod

# Default paths bundled inside the container
DEFAULT_PRESET_DIR = os.environ.get("RUNPOD_PRESET_DIR", "/usr/local/share/projectM/presets")
DEFAULT_TEXTURE_DIR = os.environ.get("RUNPOD_TEXTURE_DIR", "/usr/local/share/projectM/textures")
DEFAULT_OUTPUT_NAME = os.environ.get("RUNPOD_OUTPUT_NAME", "output.mp4")
DEFAULT_TIMEOUT_SEC = float(os.environ.get("RUNPOD_CONVERT_TIMEOUT", "10800"))  # 3 hours


class ConversionError(Exception):
    """Raised when the conversion script returns a non-zero exit code."""


def _write_binary_file(target: Path, data_b64: str) -> None:
    try:
        with target.open("wb") as file_handle:
            file_handle.write(base64.b64decode(data_b64))
    except Exception as exc:  # noqa: BLE001
        raise ConversionError(f"Failed to decode base64 payload to {target.name}: {exc}") from exc


def _build_command(
    audio_path: Path,
    output_path: Path,
    timeline_path: Path | None,
    job_input: dict,
) -> list[str]:
    width = int(job_input.get("video_width", 1920))
    height = int(job_input.get("video_height", 1080))
    fps = int(job_input.get("fps", 60))
    bitrate = int(job_input.get("bitrate_kbps", 8000))
    mesh = str(job_input.get("mesh", os.environ.get("PROJECTM_MESH", "320x240")))
    preset_duration = int(job_input.get("preset_duration", 60))
    encoder_speed = job_input.get("encoder_speed", os.environ.get("PROJECTM_ENCODER_SPEED", "veryfast"))

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
        f"{width}x{height}",
        "-r",
        str(fps),
        "-b",
        str(bitrate),
        "--speed",
        str(encoder_speed),
    ]

    if timeline_path is not None:
        cmd.extend(["--timeline", str(timeline_path)])
    else:
        cmd.extend(["-d", str(preset_duration)])

    return cmd


def handler(job):
    input_payload = job.get("input") or {}
    audio_b64 = input_payload.get("audio_b64")
    if not audio_b64:
        return {"error": "Missing audio_b64 in payload"}

    audio_name = input_payload.get("audio_filename") or "input_audio"
    suffix = Path(audio_name).suffix or ".mp3"

    timeline_ini = input_payload.get("timeline_ini")
    timeout_override = float(input_payload.get("timeout_sec") or DEFAULT_TIMEOUT_SEC)

    with tempfile.TemporaryDirectory(prefix="runpod_projectm_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        audio_path = tmp_path / f"audio{suffix}"
        _write_binary_file(audio_path, audio_b64)

        timeline_path = None
        if timeline_ini:
            timeline_path = tmp_path / "timeline.ini"
            timeline_path.write_text(timeline_ini, encoding="utf-8")

        output_path = tmp_path / DEFAULT_OUTPUT_NAME

        cmd = _build_command(audio_path, output_path, timeline_path, input_payload)

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                check=True,
                timeout=timeout_override,
            )
        except subprocess.TimeoutExpired as timeout_exc:  # noqa: PERF203
            return {
                "error": f"Conversion timed out after {timeout_override} seconds",
                "stdout": timeout_exc.stdout,
                "stderr": timeout_exc.stderr,
            }
        except subprocess.CalledProcessError as proc_exc:
            return {
                "error": f"Conversion failed (exit code {proc_exc.returncode})",
                "stdout": proc_exc.stdout,
                "stderr": proc_exc.stderr,
            }
        except Exception as exc:  # noqa: BLE001
            return {"error": f"Unexpected conversion failure: {exc}"}

        if not output_path.exists():
            return {
                "error": "Conversion completed but output file is missing",
                "stdout": result.stdout,
                "stderr": result.stderr,
            }

        output_b64 = base64.b64encode(output_path.read_bytes()).decode("utf-8")

        return {
            "base_video_b64": output_b64,
            "stdout": result.stdout,
            "stderr": result.stderr,
        }


if __name__ == "__main__":
    runpod.serverless.start({"handler": handler})
