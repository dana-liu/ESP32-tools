#!/usr/bin/env python3
"""Sync Codex quota, read runtime status, or update the terrarium over BLE."""

import argparse
import asyncio
from contextlib import asynccontextmanager, contextmanager
import fcntl
import os
from pathlib import Path
import runpy
import struct
import tempfile
import time

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

UUID_BASE = "6bdc510{}-b75f-462a-a377-8dcecc0d415d"
SERVICE, QUOTA, LOG, UPDATE, PROGRESS = (UUID_BASE.format(i) for i in range(5))
read_quota = runpy.run_path(str(Path(__file__).with_name("sync-quota.py")))["read_quota"]


@contextmanager
def radio_lock(*, wait):
    lock = Path(tempfile.gettempdir()) / f"terrarium-ble-{os.getuid()}.lock"
    with lock.open("a") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX | (0 if wait else fcntl.LOCK_NB))
        yield


@asynccontextmanager
async def connect():
    board = await BleakScanner.find_device_by_filter(
        lambda _device, advertisement: SERVICE in advertisement.service_uuids,
        timeout=8, service_uuids=[SERVICE],
    )
    if board is None:
        raise RuntimeError("找不到 Terrarium 蓝牙设备")
    async with BleakClient(board, timeout=12) as client:
        yield client


async def read_text(client, characteristic):
    async with asyncio.timeout(10):
        return (await client.read_gatt_char(characteristic)).decode().strip()


async def write(client, characteristic, data):
    async with asyncio.timeout(12):
        await client.write_gatt_char(characteristic, data, response=True)


async def sync_once(codex):
    try:
        fields, resets = await read_quota(codex, include_resets=True)
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        quota_error = error
        fields = None
    with radio_lock(wait=False):
        async with connect() as client:
            await read_text(client, LOG)
            now = int(time.time())
            offset = time.localtime(now).tm_gmtoff // 60
            await write(client, QUOTA, f"TIME {now} {offset}".encode())
            if fields is None:
                raise quota_error
            await write(client, QUOTA, ("QUOTA " + " ".join(map(str, fields))).encode())
            await write(client, QUOTA, f"RESET {resets[0]} {resets[1]}".encode())
            deadline = asyncio.get_running_loop().time() + 5
            expected = f"quota={fields[1]}/{fields[3]}"
            while expected not in await read_text(client, LOG):
                if asyncio.get_running_loop().time() >= deadline:
                    raise RuntimeError("ESP32 未确认显示额度")
                await asyncio.sleep(0.25)


async def sync(codex, once):
    while True:
        try:
            await sync_once(codex)
            print("ESP32 已确认蓝牙额度同步", flush=True)
            if once:
                return 0
        except BlockingIOError:
            if once:
                raise RuntimeError("蓝牙通道正在使用")
        except (BleakError, OSError, RuntimeError, TimeoutError) as error:
            if once:
                raise
            print(f"额度同步暂未完成：{error}", flush=True)
        await asyncio.sleep(60)


async def wait_progress(client, expected):
    deadline = asyncio.get_running_loop().time() + 12
    while asyncio.get_running_loop().time() < deadline:
        value = await read_text(client, PROGRESS)
        if value == expected:
            return
        if value.startswith("ERR"):
            raise RuntimeError(value)
        await asyncio.sleep(0.04)
    raise RuntimeError(f"等待板子确认 {expected} 超时")


async def ota(path, code):
    image = path.read_bytes()
    if not image or image[0] != 0xE9 or len(image) > 0x300000:
        raise RuntimeError("请选择大小不超过 3 MiB 的 ESP32 应用 .ino.bin")
    with radio_lock(wait=True):
        async with connect() as client:
            before = await read_text(client, LOG)
            await write(client, UPDATE, struct.pack("<BII", 1, code, len(image)))
            await wait_progress(client, "BYTES 0")
            total = len(image)
            report_at = 0
            offset = 0
            while offset < total:
                for _ in range(4):
                    chunk = image[offset:offset + 175]
                    if not chunk:
                        break
                    await write(client, UPDATE, struct.pack("<BI", 2, offset) + chunk)
                    offset += len(chunk)
                await wait_progress(client, f"BYTES {offset}")
                if offset >= report_at:
                    print(f"已传输 {100 * offset // total}%", flush=True)
                    report_at = offset + max(total // 10, 1)
            await write(client, UPDATE, b"\x03")
            await wait_progress(client, "REBOOT")
        await asyncio.sleep(3)
        async with connect() as client:
            after = await read_text(client, LOG)
        if not after or after.split(" | ")[0] == before.split(" | ")[0]:
            raise RuntimeError("板子已接收固件，但尚未确认从备用分区启动")
        print(f"无线更新成功：{after}", flush=True)


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sync_parser = sub.add_parser("sync", help="同步 Codex 额度")
    sync_parser.add_argument("--codex", default="codex")
    sync_parser.add_argument("--once", action="store_true")
    status_parser = sub.add_parser("status", help="读取当前运行状态")
    status_parser.add_argument("--watch", action="store_true", help="每两秒输出一条运行日志")
    ota_parser = sub.add_parser("ota", help="无线更新应用固件")
    ota_parser.add_argument("image", type=Path)
    ota_parser.add_argument("--code", type=int, required=True)
    args = parser.parse_args()
    try:
        if args.command == "sync":
            return await sync(args.codex, args.once)
        if args.command == "status":
            with radio_lock(wait=True):
                async with connect() as client:
                    while True:
                        status = await read_text(client, LOG)
                        update_state = await read_text(client, PROGRESS)
                        print(f"{status} | ota={update_state}", flush=True)
                        if not args.watch:
                            break
                        await asyncio.sleep(2)
        else:
            await ota(args.image, args.code)
    except (BleakError, OSError, RuntimeError, TimeoutError) as error:
        parser.exit(1, f"{error}\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(asyncio.run(main()))
    except KeyboardInterrupt:
        raise SystemExit(130)
