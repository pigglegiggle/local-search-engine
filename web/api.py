"""FastAPI reads immutable C++ snapshots; no shell commands in request handlers."""

from contextlib import asynccontextmanager
from collections import OrderedDict
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import threading
import tempfile
import time
import uuid
from typing import Annotated

from fastapi import FastAPI, File, HTTPException, Query, Request, UploadFile
from starlette.responses import JSONResponse
from pydantic import BaseModel

from web._native import NativeIndex, build_index
from web.config import MAX_BATCH_BYTES, MAX_COLLECTIONS, MAX_FILE_BYTES, MAX_FILES, TEXT_EXTENSIONS

PROJECT = Path(__file__).resolve().parents[1]


class Explanation(BaseModel):
    term: str
    tf: int
    df: int
    score: float


class Hit(BaseModel):
    path: str
    score: float
    snippet: str
    explanation: list[Explanation] | None = None


class SearchResponse(BaseModel):
    query: str
    total: int
    search_ms: float
    request_ms: float
    snapshot_id: str
    expanded_terms: list[str]
    hits: list[Hit]


class StatsResponse(BaseModel):
    root: str
    documents: int
    terms: int
    tokens: int
    postings: int
    source_bytes: int
    index_bytes: int
    load_ms: float
    snapshot_id: str
    source_changed: bool


@dataclass(frozen=True)
class Snapshot:
    engine: NativeIndex
    identity: tuple[int, int, int, int]
    stats: dict
    version: str


def file_identity(path: Path) -> tuple[int, int, int, int]:
    info = path.stat()
    return info.st_dev, info.st_ino, info.st_mtime_ns, info.st_size


class SearchService:
    def __init__(self, path: Path):
        self.path = path.absolute()
        self._lock = threading.Lock()
        self._reload_lock = threading.Lock()
        self._snapshot = self._load()

    def _load(self) -> Snapshot:
        # Detect replacements during load; metadata fingerprints identify snapshots, not authenticity.
        for _ in range(3):
            before = file_identity(self.path)
            engine = NativeIndex(str(self.path))
            after = file_identity(self.path)
            if before == after:
                version = hashlib.sha256(repr(after).encode()).hexdigest()[:16]
                stats = engine.stats()
                stats["index_bytes"] = after[3]
                return Snapshot(engine, after, stats, version)
        raise RuntimeError(
            "Index changed repeatedly during loading. Retry after indexing finishes."
        )

    def snapshot(self) -> Snapshot:
        with self._lock:
            return self._snapshot

    def stats(self) -> dict:
        snapshot = self.snapshot()
        try:
            changed = file_identity(self.path) != snapshot.identity
        except OSError:
            changed = True
        return {**snapshot.stats, "snapshot_id": snapshot.version, "source_changed": changed}

    def search(self, query: str, limit: int, explain: bool) -> dict:
        begin = time.perf_counter()
        snapshot = self.snapshot()
        result = snapshot.engine.search(query, limit, explain)
        return {
            **result,
            "snapshot_id": snapshot.version,
            "request_ms": (time.perf_counter() - begin) * 1000,
        }

    def reload(self) -> dict:
        with self._reload_lock:
            replacement = self._load()
            with self._lock:
                self._snapshot = replacement
        return self.stats()


class UploadCollections:
    """Bounded temporary collections; failed imports never replace an existing collection."""

    def __init__(self):
        self._items = OrderedDict()
        self._lock = threading.Lock()
        self._create_lock = threading.Lock()

    def get(self, identifier: str) -> SearchService:
        with self._lock:
            if identifier not in self._items:
                raise HTTPException(404, "Uploaded collection expired. Upload the files again.")
            self._items.move_to_end(identifier)
            return self._items[identifier][1]

    def create(self, files: list[UploadFile]) -> dict:
        if not files or len(files) > MAX_FILES:
            raise HTTPException(422, f"Upload between 1 and {MAX_FILES} files.")
        names = set()
        for file in files:
            name = file.filename or ""
            if (
                not name
                or name in {".", ".."}
                or "/" in name
                or "\\" in name
                or any(ord(c) < 32 or ord(c) == 127 for c in name)
                or len(name.encode("utf-8")) > 240
            ):
                raise HTTPException(
                    422, "Use simple filenames without directories or control characters."
                )
            if Path(name).suffix.lower().lstrip(".") not in TEXT_EXTENSIONS:
                raise HTTPException(
                    422, f"Unsupported file type: {name}. Upload UTF-8 text or source files."
                )
            if name.casefold() in names:
                raise HTTPException(422, "Duplicate filenames are not allowed in one upload.")
            names.add(name.casefold())
        with self._create_lock:
            temporary = tempfile.TemporaryDirectory(prefix="lse-upload-")
            try:
                directory = Path(temporary.name)
                corpus = directory / "files"
                corpus.mkdir()
                total = 0
                for file in files:
                    data = file.file.read(MAX_FILE_BYTES + 1)
                    if len(data) > MAX_FILE_BYTES:
                        raise HTTPException(413, "Each file must be at most 8 MiB.")
                    total += len(data)
                    if total > MAX_BATCH_BYTES:
                        raise HTTPException(413, "The upload must be at most 32 MiB in total.")
                    try:
                        data.decode("utf-8")
                    except UnicodeDecodeError as error:
                        raise HTTPException(
                            422, f"{file.filename} is not valid UTF-8 text."
                        ) from error
                    if b"\0" in data:
                        raise HTTPException(422, f"{file.filename} contains binary data.")
                    (corpus / file.filename).write_bytes(data)
                destination = directory / "index.lsi"
                build_index(str(corpus), str(destination))
                service = SearchService(destination)
                if service.stats()["documents"] != len(files):
                    raise HTTPException(422, "Some uploaded files could not be indexed.")
                identifier = uuid.uuid4().hex
                evicted = None
                with self._lock:
                    self._items[identifier] = (temporary, service)
                    if len(self._items) > MAX_COLLECTIONS:
                        _, evicted = self._items.popitem(last=False)
                if evicted:
                    evicted[0].cleanup()
                return {"collection_id": identifier, **service.stats()}
            except BaseException:
                temporary.cleanup()
                raise

    def close(self):
        with self._lock:
            items = list(self._items.values())
            self._items.clear()
        for temporary, _ in items:
            temporary.cleanup()


class UploadBodyLimit:
    """Bound multipart bodies before the framework spools uploaded files to disk."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if (
            scope["type"] != "http"
            or scope.get("path") != "/collections"
            or scope.get("method") != "POST"
        ):
            return await self.app(scope, receive, send)
        chunks = []
        total = 0
        while True:
            message = await receive()
            if message["type"] == "http.disconnect":
                return
            total += len(message.get("body", b""))
            if total > MAX_BATCH_BYTES + 1024 * 1024:
                response = JSONResponse(
                    {"detail": "Upload request exceeds 33 MiB."}, status_code=413
                )
                return await response(scope, receive, send)
            chunks.append(message)
            if not message.get("more_body", False):
                break
        cursor = 0

        async def bounded_receive():
            nonlocal cursor
            if cursor < len(chunks):
                message = chunks[cursor]
                cursor += 1
                return message
            return await receive()

        return await self.app(scope, bounded_receive, send)


def create_app(index_path: Path | None = None) -> FastAPI:
    path = index_path or Path(os.environ.get("LSE_INDEX", str(PROJECT / "build/demo.lsi")))

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        app.state.service = SearchService(path)
        app.state.uploads = UploadCollections()
        try:
            yield
        finally:
            app.state.uploads.close()
            del app.state.service

    application = FastAPI(
        title="Local Search Engine",
        version="1.0.0",
        description="Resident-memory C++ search. Local use; index paths are configured at startup.",
        lifespan=lifespan,
    )
    application.add_middleware(UploadBodyLimit)

    def service_for(request: Request, collection: str | None) -> SearchService:
        return (
            request.app.state.uploads.get(collection) if collection else request.app.state.service
        )

    @application.post("/collections", status_code=201)
    def upload(request: Request, files: Annotated[list[UploadFile], File()]):
        try:
            return request.app.state.uploads.create(files)
        except (OSError, RuntimeError) as error:
            raise HTTPException(409, "Could not index uploaded files. " + str(error)) from error
        finally:
            for file in files:
                file.file.close()

    @application.get("/health")
    def health(request: Request):
        snapshot = request.app.state.service.snapshot()
        return {"status": "ok", "engine": "C++17 / pybind11", "snapshot_id": snapshot.version}

    @application.get("/stats", response_model=StatsResponse)
    def stats(
        request: Request, collection: Annotated[str | None, Query(pattern="^[a-f0-9]{32}$")] = None
    ):
        return service_for(request, collection).stats()

    @application.get("/search", response_model=SearchResponse, response_model_exclude_none=True)
    def search(
        request: Request,
        q: Annotated[
            str,
            Query(
                min_length=1,
                max_length=4096,
                description="Engine query, including Boolean/phrase/fuzzy syntax",
            ),
        ],
        k: Annotated[int, Query(ge=1, le=1000)] = 10,
        explain: bool = False,
        collection: Annotated[str | None, Query(pattern="^[a-f0-9]{32}$")] = None,
    ):
        try:
            if not q.strip() or len(q.encode("utf-8")) > 4096:
                raise ValueError("Query must be nonempty and at most 4096 UTF-8 bytes.")
            return service_for(request, collection).search(q, k, explain)
        except (ValueError, UnicodeError) as error:
            raise HTTPException(status_code=422, detail=str(error)) from error

    @application.post("/reload", response_model=StatsResponse)
    def reload(request: Request):
        try:
            return request.app.state.service.reload()
        except (RuntimeError, OSError) as error:
            raise HTTPException(
                status_code=409,
                detail="Reload failed; previous snapshot remains active. " + str(error),
            ) from error

    return application


app = create_app()
