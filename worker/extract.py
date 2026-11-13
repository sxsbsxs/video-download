#!/usr/bin/env python3
"""yt-dlp metadata extractor. It never downloads the media file."""

from __future__ import annotations

import argparse
import json
import math
import sys
from typing import Any
from urllib.parse import urlsplit

from yt_dlp import YoutubeDL


def emit(payload: dict[str, Any], exit_code: int = 0) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False, separators=(",", ":"), allow_nan=False))
    sys.stdout.flush()
    raise SystemExit(exit_code)


def safe_http_url(value: Any) -> bool:
    if not isinstance(value, str) or len(value) > 12000:
        return False
    try:
        parsed = urlsplit(value)
        if parsed.scheme.lower() not in ("http", "https"):
            return False
        if not parsed.hostname or parsed.username or parsed.password:
            return False
        return True
    except Exception:
        return False


def safe_number(value: Any) -> int | None:
    try:
        if value is None:
            return None
        number = float(value)
        if not math.isfinite(number) or number < 0:
            return None
        return int(number)
    except (TypeError, ValueError, OverflowError):
        return None


def pick_thumbnail(info: dict[str, Any]) -> str | None:
    candidates = info.get("thumbnails") or []
    if isinstance(candidates, list):
        valid = [
            item for item in candidates
            if isinstance(item, dict) and safe_http_url(item.get("url"))
        ]
        valid.sort(
            key=lambda item: (safe_number(item.get("width")) or 0)
            * (safe_number(item.get("height")) or 0),
            reverse=True,
        )
        if valid:
            return valid[0].get("url")
    direct = info.get("thumbnail")
    return direct if safe_http_url(direct) else None


def normalize_format(item: dict[str, Any]) -> dict[str, Any] | None:
    media_url = item.get("url")
    if not safe_http_url(media_url):
        return None

    height = safe_number(item.get("height"))
    width = safe_number(item.get("width"))
    filesize = safe_number(item.get("filesize")) or safe_number(item.get("filesize_approx"))
    video_codec = str(item.get("vcodec") or "none")
    audio_codec = str(item.get("acodec") or "none")
    has_video = video_codec.lower() != "none"
    has_audio = audio_codec.lower() != "none"

    if width and height:
        resolution = f"{width}x{height}"
    elif height:
        resolution = f"{height}p"
    elif has_audio and not has_video:
        resolution = "仅音频"
    else:
        resolution = str(item.get("resolution") or "未知")

    tbr = item.get("tbr")
    try:
        bitrate = float(tbr) if tbr is not None else 0.0
        if not math.isfinite(bitrate):
            bitrate = 0.0
    except (TypeError, ValueError):
        bitrate = 0.0

    return {
        "format_id": str(item.get("format_id") or ""),
        "ext": str(item.get("ext") or ""),
        "format_note": str(item.get("format_note") or ""),
        "resolution": resolution,
        "width": width,
        "height": height,
        "filesize": filesize,
        "fps": safe_number(item.get("fps")),
        "tbr": int(bitrate) if bitrate > 0 else None,
        "vcodec": video_codec,
        "acodec": audio_codec,
        "has_video": has_video,
        "has_audio": has_audio,
        "protocol": str(item.get("protocol") or ""),
        "url": media_url,
        "_rank": (1 if has_video and has_audio else 0, height or 0, bitrate),
    }


def normalize_info(info: dict[str, Any]) -> dict[str, Any]:
    if info.get("_type") in ("playlist", "multi_video") or info.get("entries"):
        entries = info.get("entries") or []
        first = next((entry for entry in entries if isinstance(entry, dict)), None)
        if first:
            info = first
        else:
            raise ValueError("这个链接没有可解析的视频条目。请直接使用单个视频页面链接。")

    formats: list[dict[str, Any]] = []
    raw_formats = info.get("formats") or []
    if isinstance(raw_formats, list):
        for item in raw_formats:
            if isinstance(item, dict):
                normalized = normalize_format(item)
                if normalized:
                    formats.append(normalized)

    if not formats and isinstance(info.get("url"), str):
        fallback = normalize_format({
            "format_id": info.get("format_id"),
            "ext": info.get("ext"),
            "resolution": info.get("resolution"),
            "width": info.get("width"),
            "height": info.get("height"),
            "filesize": info.get("filesize"),
            "vcodec": info.get("vcodec"),
            "acodec": info.get("acodec"),
            "url": info.get("url"),
            "protocol": info.get("protocol"),
        })
        if fallback:
            formats.append(fallback)

    formats.sort(key=lambda item: item["_rank"], reverse=True)
    selected = formats[:32]
    for item in selected:
        item.pop("_rank", None)

    title = str(info.get("title") or "未命名视频").strip()[:300]
    description = str(info.get("description") or "").strip()[:700]
    uploader = str(info.get("uploader") or info.get("channel") or "")[:160]
    duration = safe_number(info.get("duration"))

    return {
        "title": title,
        "description": description,
        "thumbnail": pick_thumbnail(info),
        "duration": duration,
        "uploader": uploader,
        "webpage_url": info.get("webpage_url") if safe_http_url(info.get("webpage_url")) else None,
        "extractor": str(info.get("extractor_key") or info.get("extractor") or "")[:100],
        "formats": selected,
    }


def friendly_error(error: Exception) -> str:
    raw = str(error)
    lowered = raw.lower()
    if "412" in raw:
        return "目标网站返回 HTTP 412，拒绝了当前请求或要求额外验证。可稍后重试或检查网络；本工具不会绕过网站访问限制。"
    if "403" in raw:
        return "目标网站返回 HTTP 403，当前请求被拒绝。可能需要网站允许的访问条件或登录权限。"
    if "404" in raw:
        return "目标视频不存在、已删除，或链接地址不正确。"
    if "private video" in lowered or "login" in lowered or "sign in" in lowered:
        return "该内容可能需要登录或具备访问权限。当前公共版本不接收 Cookie，请使用你有权访问的公开链接。"
    if "unsupported url" in lowered:
        return "暂不支持该网站或该链接格式。请检查链接是否为视频页面。"
    if "timed out" in lowered or "timeout" in lowered:
        return "目标网站响应超时，请稍后重试。"
    if "geo" in lowered or "not available in your country" in lowered:
        return "该内容可能存在地区访问限制。"
    return "解析失败：" + raw[:280]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    args = parser.parse_args()
    url = args.url.strip()

    if not safe_http_url(url):
        emit({"ok": False, "error": "只支持有效的 HTTP 或 HTTPS 视频链接。"}, 2)

    options = {
        "quiet": True,
        "no_warnings": True,
        "noprogress": True,
        "skip_download": True,
        "noplaylist": True,
        "socket_timeout": 15,
        "retries": 1,
        "extractor_retries": 2,
        "fragment_retries": 1,
        "cachedir": False,
        "no_color": True,
        "js_runtimes": {"node": {}},
    }

    try:
        with YoutubeDL(options) as ydl:
            info = ydl.extract_info(url, download=False)
        if not isinstance(info, dict):
            raise ValueError("网站没有返回可用的视频信息。")
        data = normalize_info(info)
        if not data["formats"]:
            raise ValueError("已取得视频信息，但网站没有提供可用的直连媒体格式。")
        emit({"ok": True, "data": data})
    except SystemExit:
        raise
    except Exception as error:
        emit({"ok": False, "error": friendly_error(error)}, 2)


if __name__ == "__main__":
    main()
