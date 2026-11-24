"""Parse Gemini model-group quota rows from the Antigravity accessibility reader."""

import asyncio
import json
import os
from pathlib import Path
import re
import time

ROWS = ("Five Hour Limit Remaining", "Weekly Limit Remaining")
UNIT_SECONDS = {"day": 86400, "hour": 3600, "minute": 60, "second": 1}
CACHE_PATH = Path.home() / ".cache" / "esp32-terrarium" / "antigravity-quota.json"
NO_RESET_TTL = 86400


def _load_cache(cache_path=CACHE_PATH):
    try:
        data = json.loads(cache_path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def _save_cache(fields, resets, previous, now, cache_path=CACHE_PATH):
    payload = {
        "fields": list(fields),
        "resets": list(resets),
        "previous": {k: list(v) for k, v in previous.items()},
        "updated_at": now,
    }
    try:
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = cache_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, cache_path)
    except OSError:
        pass


def _cached_result(now, cache_path=CACHE_PATH):
    data = _load_cache(cache_path)
    if not data:
        return None
    raw_fields = data.get("fields")
    raw_resets = data.get("resets")
    updated_at = data.get("updated_at", 0)
    if not (isinstance(raw_fields, list) and len(raw_fields) == 2 and
            isinstance(raw_resets, list) and len(raw_resets) == 2):
        return None
    fields = []
    resets = []
    for remaining, reset in zip(raw_fields, raw_resets):
        if type(remaining) is not int or not 0 <= remaining <= 100 or type(reset) is not int or reset < 0:
            fields.append(-1)
            resets.append(0)
        elif reset > 0:
            if reset > now:
                fields.append(remaining)
                resets.append(reset)
            else:
                fields.append(-1)
                resets.append(0)
        elif type(updated_at) is int and 0 <= now - updated_at <= NO_RESET_TTL:
            fields.append(remaining)
            resets.append(0)
        else:
            fields.append(-1)
            resets.append(0)
    if fields == [-1, -1]:
        return None
    return (*fields, *resets)


def parse_gemini_panel(panel, now, previous):
    fields = []
    resets = []
    for label in ROWS:
        row = next((line.strip() for line in panel.splitlines() if label in line), "")
        percentages = re.findall(r"(?<![\d.])(?:100|[0-9]{1,2})\s*%", row)
        if not percentages:
            fields.append(-1)
            resets.append(0)
            continue
        remaining = int(percentages[-1].strip("% "))
        countdown = re.search(r"(?:refresh|reset)(?:es)?\s+in\s+(.+)", row, re.I)
        duration = sum(int(amount) * UNIT_SECONDS[unit.lower()] for amount, unit in
                       re.findall(r"(\d+)\s+(day|hour|minute|second)s?\b",
                                  countdown.group(1) if countdown else "", re.I))
        if label in previous and previous[label][0] == row:
            reset = previous[label][1]
        else:
            reset = now + duration if duration else 0
            previous[label] = (row, reset)
        fields.append(remaining if not reset or reset > now else -1)
        resets.append(reset if reset > now else 0)
    if fields == [-1, -1]:
        raise ValueError("Gemini 额度不可读或快照已过期，请刷新 Models & Usage")
    return (*fields, *resets)


async def read_gemini(reader, previous, *, cache_path=CACHE_PATH):
    now = int(time.time())
    if not previous:
        cached = _load_cache(cache_path)
        if cached and isinstance(cached.get("previous"), dict):
            for label in ROWS:
                entry = cached["previous"].get(label)
                if (isinstance(entry, list) and len(entry) == 2
                        and isinstance(entry[0], str) and type(entry[1]) is int and entry[1] >= 0):
                    previous[label] = (entry[0], entry[1])
    try:
        process = await asyncio.create_subprocess_exec(
            reader, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
        try:
            stdout, stderr = await asyncio.wait_for(process.communicate(), timeout=8)
        except TimeoutError:
            try:
                process.kill()
            except ProcessLookupError:
                pass
            await process.communicate()
            raise RuntimeError("读取 Antigravity 额度超时") from None
        if process.returncode:
            raise RuntimeError(stderr.decode().strip() or "Antigravity 额度读取失败")
        result = parse_gemini_panel(stdout.decode(), now, previous)
        _save_cache(result[:2], result[2:], previous, now, cache_path)
        return result
    except (OSError, RuntimeError, ValueError):
        fallback = _cached_result(now, cache_path)
        if fallback is not None:
            return fallback
        raise
