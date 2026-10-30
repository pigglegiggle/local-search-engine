#!/usr/bin/env python3
"""Run owned API/UI processes together and stop both on Ctrl+C."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

import httpx

root = Path(__file__).resolve().parents[1]
children: list[subprocess.Popen] = []


def ensure_free(port: int):
    with socket.socket() as sock:
        try:
            sock.bind(("127.0.0.1", port))
        except OSError as error:
            raise RuntimeError(f"Port {port} is already in use. Stop that service or run make api / make ui separately.") from error


def start(arguments: list[str], env: dict):
    process = subprocess.Popen([sys.executable, *arguments], cwd=root, env=env)
    children.append(process)
    return process


def main() -> int:
    ensure_free(8000)
    ensure_free(8501)
    env = {**os.environ, "LSE_API_URL": "http://127.0.0.1:8000"}
    api = start(["-m", "uvicorn", "web.api:app", "--host", "127.0.0.1", "--port", "8000"], env)
    deadline = time.monotonic() + 30
    with httpx.Client(timeout=1, trust_env=False) as client:
        while time.monotonic() < deadline:
            if api.poll() is not None:
                raise RuntimeError("Search API exited during startup; see the error above.")
            try:
                if client.get("http://127.0.0.1:8000/health").is_success:
                    break
            except httpx.RequestError:
                pass
            time.sleep(.1)
        else:
            raise RuntimeError("Search API did not become ready within 30 seconds.")
    start(["-m", "streamlit", "run", "web/ui.py", "--server.address", "127.0.0.1",
           "--server.port", "8501", "--server.headless", "true"], env)
    print("\nSearch UI: http://127.0.0.1:8501\nAPI docs:  http://127.0.0.1:8000/docs\nCtrl+C stops both services.\n", flush=True)
    while all(child.poll() is None for child in children):
        time.sleep(.25)
    raise RuntimeError("A web service stopped; shutting down its companion.")


if __name__ == "__main__":
    try:
        exit_code = main()
    except KeyboardInterrupt:
        exit_code = 0
    except (RuntimeError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        exit_code = 1
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
        for child in children:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    sys.exit(exit_code)
