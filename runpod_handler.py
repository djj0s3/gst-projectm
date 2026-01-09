import base64
import html
import json
import logging
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path
from urllib.parse import parse_qsl, urlencode, urlparse, urlunparse

import requests
import runpod

# Default paths bundled inside the container
DEFAULT_PRESET_DIR = os.environ.get("RUNPOD_PRESET_DIR", "/usr/local/share/projectM/presets")
DEFAULT_TEXTURE_DIR = os.environ.get("RUNPOD_TEXTURE_DIR", "/usr/local/share/projectM/textures")
DEFAULT_OUTPUT_NAME = os.environ.get("RUNPOD_OUTPUT_NAME", "output.mp4")
DEFAULT_TIMEOUT_SEC = float(os.environ.get("RUNPOD_CONVERT_TIMEOUT", "10800"))  # 3 hours
DEFAULT_DOWNLOAD_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36"
    )
}
DIRECT_AUDIO_HINTS = (
    "download.aspx",
    ".files.1drv.com",
    ".download.",
    ".mp3",
    ".wav",
    ".flac",
    ".m4a",
    "drive.google.com/uc",
    "googleusercontent.com",
)
LOG_LEVEL = os.environ.get("RUNPOD_LOG_LEVEL", "INFO").upper()
LOG_STD_TAIL = int(os.environ.get("RUNPOD_LOG_STD_TAIL", "1200"))

LOGGER = logging.getLogger("projectm.runpod")
if not LOGGER.handlers:
    handler = logging.StreamHandler()
    handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s"))
    LOGGER.addHandler(handler)

try:
    LOGGER.setLevel(getattr(logging, LOG_LEVEL))
except AttributeError:
    LOGGER.setLevel(logging.INFO)
LOGGER.propagate = False


class ConversionError(Exception):
    """Raised when the conversion script returns a non-zero exit code."""


def _write_binary_file(target: Path, data_b64: str) -> None:
    try:
        with target.open("wb") as file_handle:
            file_handle.write(base64.b64decode(data_b64))
    except Exception as exc:  # noqa: BLE001
        raise ConversionError(f"Failed to decode base64 payload to {target.name}: {exc}") from exc


def _normalize_storage_url(url: str) -> str:
    """Coerce known providers (Dropbox/OneDrive) into direct-download URLs."""
    parsed = urlparse(url)
    host = parsed.netloc.lower()
    query = dict(parse_qsl(parsed.query, keep_blank_values=True))

    if "dropbox.com" in host:
        # Force direct download
        query["dl"] = "1"
        parsed = parsed._replace(netloc="www.dropbox.com", query=urlencode(query))
    elif "onedrive" in host or "1drv.ms" in host or "sharepoint.com" in host:
        query["download"] = "1"
        parsed = parsed._replace(query=urlencode(query))
    elif "drive.google.com" in host or "docs.google.com" in host:
        file_id = None
        path_parts = [part for part in parsed.path.split("/") if part]
        if len(path_parts) >= 3 and path_parts[0] == "file" and path_parts[1] == "d":
            file_id = path_parts[2]
        elif "id" in query:
            file_id = query["id"]
        if file_id:
            parsed = parsed._replace(
                scheme="https",
                netloc="drive.google.com",
                path="/uc",
                query=urlencode({"export": "download", "id": file_id}),
            )

    return urlunparse(parsed)


def _extract_direct_audio_url(html_text: str) -> str | None:
    def _clean(candidate: str) -> str:
        decoded = html.unescape(candidate)
        try:
            decoded = bytes(decoded, "utf-8").decode("unicode_escape")
        except UnicodeDecodeError:
            pass
        return decoded

    def _maybe_build_drive_confirm_url() -> str | None:
        confirm_match = re.search(r'name=["\']confirm["\']\s+value=["\']([^"\']+)["\']', html_text, re.IGNORECASE)
        id_match = re.search(r'name=["\']id["\']\s+value=["\']([^"\']+)["\']', html_text, re.IGNORECASE)
        if not (confirm_match and id_match):
            return None
        confirm_token = _clean(confirm_match.group(1))
        file_id = _clean(id_match.group(1))
        query = urlencode({"export": "download", "confirm": confirm_token, "id": file_id})
        return f"https://drive.google.com/uc?{query}"

    patterns = [
        re.compile(r'window\.location(?:\.replace|\.href)?\s*\(\s*[\'"]([^\'"]+)[\'"]', re.IGNORECASE),
        re.compile(r'content\s*=\s*"\d+;\s*url=([^"]+)"', re.IGNORECASE),
    ]
    for pattern in patterns:
        match = pattern.search(html_text)
        if match:
            return _clean(match.group(1))

    google_patterns = [
        re.compile(r'href="(https://drive\.google\.com/uc\?[^"]+)"', re.IGNORECASE),
        re.compile(r'"downloadUrl":"(https:[^"]+)"', re.IGNORECASE),
        re.compile(r'data-url="(https://[^"]+googleusercontent\.com[^"]+)"', re.IGNORECASE),
    ]
    for pattern in google_patterns:
        match = pattern.search(html_text)
        if match:
            return _clean(match.group(1))

    drive_confirm = _maybe_build_drive_confirm_url()
    if drive_confirm:
        return drive_confirm

    candidates = re.findall(r'https?://[^\s"\']+', html_text)
    for candidate in candidates:
        cleaned = _clean(candidate)
        lowered = cleaned.lower()
        if any(hint in lowered for hint in DIRECT_AUDIO_HINTS):
            return cleaned
    return None


def _download_from_url(audio_url: str, target: Path) -> None:
    normalized = _normalize_storage_url(audio_url)
    session = requests.Session()
    url = normalized

    for _ in range(4):
        with session.get(
            url,
            stream=True,
            timeout=120,
            allow_redirects=True,
            headers=DEFAULT_DOWNLOAD_HEADERS,
        ) as response:
            response.raise_for_status()
            content_type = response.headers.get("Content-Type", "") or ""
            if content_type.lower().startswith("audio/"):
                with target.open("wb") as out_file:
                    total = 0
                    for chunk in response.iter_content(chunk_size=1024 * 1024):
                        if not chunk:
                            continue
                        total += len(chunk)
                        out_file.write(chunk)
                if os.path.getsize(target) == 0:
                    target.unlink(missing_ok=True)
                    raise ConversionError("Remote download returned an empty file.")
                return

            html_text = response.content.decode("utf-8", errors="ignore")
        direct_url = _extract_direct_audio_url(html_text)
        if direct_url:
            url = direct_url
            continue
        raise ConversionError(
            "Remote URL returned HTML or requires authentication. Ensure the link is publicly accessible."
        )

    raise ConversionError("Could not resolve remote audio after multiple attempts.")


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


def _tail_text(text: str | None, max_chars: int = LOG_STD_TAIL) -> str:
    if not text:
        return ""
    if len(text) <= max_chars:
        return text
    return f"...{text[-max_chars:]}"


def handler(job):
    job_id = None
    try:
        if isinstance(job, dict):
            job_id = job.get("id")
        LOGGER.info("Received RunPod request%s", f" {job_id}" if job_id else "")
        LOGGER.info("Job input: %s", job)

        input_payload = job.get("input") or {}
        audio_b64 = input_payload.get("audio_b64")
        audio_url = input_payload.get("audio_url")
        if not audio_b64 and not audio_url:
            LOGGER.error("Job %s missing audio_b64/audio_url input", job_id or "unknown")
            error_result = {"error": "Missing audio_b64 or audio_url in payload"}
            LOGGER.error("Job %s - returning error: %s", job_id or "unknown", error_result)
            return error_result

        audio_name = input_payload.get("audio_filename") or "input_audio"
        suffix = Path(audio_name).suffix or ".mp3"

        timeline_ini = input_payload.get("timeline_ini")
        timeout_override = float(input_payload.get("timeout_sec") or DEFAULT_TIMEOUT_SEC)
        LOGGER.info(
            "Job %s - timeout %.0f sec, mesh=%s, fps=%s, bitrate=%s kbps",
            job_id or "unknown",
            timeout_override,
            input_payload.get("mesh") or os.environ.get("PROJECTM_MESH", "320x240"),
            input_payload.get("fps", 60),
            input_payload.get("bitrate_kbps", 8000),
        )

        with tempfile.TemporaryDirectory(prefix="runpod_projectm_") as tmp_dir:
            tmp_path = Path(tmp_dir)
            audio_path = tmp_path / f"audio{suffix}"
            if audio_b64:
                LOGGER.info("Job %s - received inline audio payload", job_id or "unknown")
                _write_binary_file(audio_path, audio_b64)
            elif audio_url:
                LOGGER.info("Job %s - downloading audio from %s", job_id or "unknown", audio_url)
                try:
                    _download_from_url(audio_url, audio_path)
                except ConversionError as exc:
                    LOGGER.error("Job %s - download failed: %s", job_id or "unknown", exc)
                    error_result = {"error": f"Remote download failed: {exc}"}
                    LOGGER.error("Job %s - returning error: %s", job_id or "unknown", error_result)
                    return error_result
                LOGGER.info("Job %s - download complete (%d bytes)", job_id or "unknown", audio_path.stat().st_size)

            timeline_path = None
            if timeline_ini:
                LOGGER.info("Job %s - using inline timeline.ini payload", job_id or "unknown")
                timeline_path = tmp_path / "timeline.ini"
                timeline_path.write_text(timeline_ini, encoding="utf-8")

            output_path = tmp_path / DEFAULT_OUTPUT_NAME

            cmd = _build_command(audio_path, output_path, timeline_path, input_payload)
            LOGGER.info("Job %s - executing convert.sh command: %s", job_id or "unknown", " ".join(cmd))

            try:
                start_time = time.perf_counter()
                # Use Popen with real-time output to avoid buffering issues
                process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )

                # Read output in real-time to prevent pipe buffer deadlocks
                stdout_lines = []
                stderr_lines = []

                # Wait for process to complete with timeout
                try:
                    stdout, stderr = process.communicate(timeout=timeout_override)
                    stdout_lines = stdout.splitlines() if stdout else []
                    stderr_lines = stderr.splitlines() if stderr else []
                    returncode = process.returncode
                except subprocess.TimeoutExpired:
                    process.kill()
                    stdout, stderr = process.communicate()
                    raise

                # Create result object compatible with subprocess.run()
                class Result:
                    def __init__(self, returncode, stdout, stderr):
                        self.returncode = returncode
                        self.stdout = stdout
                        self.stderr = stderr

                result = Result(returncode, stdout, stderr)

                if result.returncode != 0:
                    raise subprocess.CalledProcessError(result.returncode, cmd, result.stdout, result.stderr)
                elapsed = time.perf_counter() - start_time
                LOGGER.info("Job %s - convert.sh completed in %.1fs", job_id or "unknown", elapsed)

                # Log stdout/stderr even on success for debugging
                if result.stdout:
                    LOGGER.info("Job %s - STDOUT: %s", job_id or "unknown", _tail_text(result.stdout, 2000))
                if result.stderr:
                    LOGGER.info("Job %s - STDERR: %s", job_id or "unknown", _tail_text(result.stderr, 2000))
            except subprocess.TimeoutExpired as timeout_exc:  # noqa: PERF203
                LOGGER.error("Job %s - conversion timed out after %.0f seconds", job_id or "unknown", timeout_override)
                return {
                    "error": f"Conversion timed out after {timeout_override} seconds",
                    "stdout": timeout_exc.stdout,
                    "stderr": timeout_exc.stderr,
                }
            except subprocess.CalledProcessError as proc_exc:
                LOGGER.error(
                    "Job %s - conversion failed with exit code %s", job_id or "unknown", proc_exc.returncode
                )
                # Log the actual error output to help debug
                if proc_exc.stdout:
                    LOGGER.error("Job %s - STDOUT: %s", job_id or "unknown", _tail_text(proc_exc.stdout))
                if proc_exc.stderr:
                    LOGGER.error("Job %s - STDERR: %s", job_id or "unknown", _tail_text(proc_exc.stderr))

                return {
                    "error": f"Conversion failed (exit code {proc_exc.returncode})",
                    "stdout": proc_exc.stdout,
                    "stderr": proc_exc.stderr,
                }
            except Exception as exc:  # noqa: BLE001
                LOGGER.exception("Job %s - unexpected conversion failure", job_id or "unknown")
                return {"error": f"Unexpected conversion failure: {exc}"}

            if not output_path.exists():
                LOGGER.error("Job %s - output missing after conversion", job_id or "unknown")
                return {
                    "error": "Conversion completed but output file is missing",
                    "stdout": result.stdout,
                    "stderr": result.stderr,
                }

            # Upload video to Runpod's CDN instead of base64 encoding
            file_size_mb = output_path.stat().st_size / (1024 * 1024)
            LOGGER.info("Job %s - uploading video to Runpod CDN (%.2f MB)", job_id or "unknown", file_size_mb)

            try:
                # Upload file using runpod.upload_file()
                video_url = runpod.upload_file(str(output_path))
                LOGGER.info("Job %s - video uploaded successfully: %s", job_id or "unknown", video_url)

                success_result = {
                    "video_url": video_url,
                    "file_size_mb": round(file_size_mb, 2),
                    "stdout": result.stdout,
                    "stderr": result.stderr,
                }
                LOGGER.info("Job %s - returning success result with video_url", job_id or "unknown")
                return success_result
            except Exception as upload_exc:  # noqa: BLE001
                LOGGER.error("Job %s - failed to upload video to Runpod CDN: %s", job_id or "unknown", upload_exc)
                # Fallback to base64 if upload fails
                LOGGER.info("Job %s - falling back to base64 encoding", job_id or "unknown")
                output_b64 = base64.b64encode(output_path.read_bytes()).decode("utf-8")
                fallback_result = {
                    "base_video_b64": output_b64,
                    "file_size_mb": round(file_size_mb, 2),
                    "upload_error": str(upload_exc),
                    "stdout": result.stdout,
                    "stderr": result.stderr,
                }
                LOGGER.info("Job %s - returning fallback result with base64", job_id or "unknown")
                return fallback_result
    except Exception as exc:  # noqa: BLE001
        LOGGER.exception("Job %s - handler crashed", job_id or "unknown")
        error_result = {"error": f"RunPod handler crashed: {exc}"}
        LOGGER.error("Job %s - returning error result: %s", job_id or "unknown", error_result)
        return error_result


if __name__ == "__main__":
    runpod.serverless.start({"handler": handler})
