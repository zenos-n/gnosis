#!/usr/bin/env python3
"""Run the native fixture against the real local aiohttp rendezvous/relay server."""
import argparse
import asyncio
import os
import runpy
import tempfile
from aiohttp import web


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("server", help="Path to gnosis-server/server.py")
    args = parser.parse_args()
    application = runpy.run_path(args.server)["create_app"]()
    runner = web.AppRunner(application, access_log=None)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    try:
        with tempfile.TemporaryDirectory(prefix="gnosis-shared-transport-") as temporary:
            for mode in ("relay", "p2p"):
                process = await asyncio.create_subprocess_exec(
                    args.binary, f"ws://127.0.0.1:{port}", mode,
                    env={**os.environ, "HOME": temporary},
                )
                try:
                    code = await asyncio.wait_for(process.wait(), timeout=60)
                    if code:
                        raise RuntimeError(f"{mode} transport fixture exited {code}")
                finally:
                    if process.returncode is None:
                        process.terminate()
                        await process.wait()
    finally:
        await runner.cleanup()


if __name__ == "__main__":
    asyncio.run(main())
