"""HTTP-only client shared by the Streamlit UI and its tests."""

import os
import time

import httpx


class APIError(RuntimeError):
    pass


def call_api(
    path: str, *, method: str = "GET", params: dict | None = None, files: list | None = None
) -> dict:
    base = os.environ.get("LSE_API_URL", "http://127.0.0.1:8000").rstrip("/")
    begin = time.perf_counter()
    try:
        response = httpx.request(
            method, base + path, params=params, files=files, timeout=60, trust_env=False
        )
    except httpx.RequestError as error:
        raise APIError(
            "Cannot connect to the Search API. Start it with make api, then retry."
        ) from error
    if response.is_error:
        try:
            detail = response.json().get("detail", "API request failed")
        except ValueError:
            detail = "API request failed"
        if isinstance(detail, list):
            detail = "; ".join(item.get("msg", "Invalid request") for item in detail)
        raise APIError(str(detail))
    result = response.json()
    result["round_trip_ms"] = (time.perf_counter() - begin) * 1000
    return result
