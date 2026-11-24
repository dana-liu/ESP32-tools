"""Parse Gemini model-group quota rows from the Antigravity accessibility reader."""

import asyncio
import re
import time

ROWS = ("Five Hour Limit Remaining", "Weekly Limit Remaining")
UNIT_SECONDS = {"day": 86400, "hour": 3600, "minute": 60, "second": 1}


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


async def read_gemini(reader, previous):
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
    return parse_gemini_panel(stdout.decode(), int(time.time()), previous)
