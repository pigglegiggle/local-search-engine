"""HTTP contracts, native parity, snapshot lifetime, and real Streamlit widget flows."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess

from fastapi.testclient import TestClient
import pytest
from streamlit.testing.v1 import AppTest

from web.api import create_app
from web.client import APIError

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/lse"


@pytest.fixture
def system(tmp_path):
    corpus = tmp_path / "corpus"
    corpus.mkdir()
    (corpus / "a.md").write_text("memory allocation cache cache", encoding="utf-8")
    (corpus / "b.cpp").write_text("allocation memory scheduler", encoding="utf-8")
    (corpus / "empty.txt").write_text("")
    (corpus / "ภาษาไทย.md").write_text("ไทย สวัสดี", encoding="utf-8")
    index = tmp_path / "corpus.lsi"

    def rebuild():
        subprocess.run([str(BINARY), "index", str(corpus), "--index", str(index), "--rebuild"],
                       check=True, capture_output=True, text=True)

    rebuild()
    app = create_app(index)
    with TestClient(app) as client:
        yield app, client, corpus, index, rebuild


@pytest.mark.parametrize("query", ["memory", '"memory allocation"', "memry~1", "sched*",
                                   "(cache OR memory) NOT ext:cpp", "NOT memory", "ไทย"])
def test_search_matches_cli(system, query):
    app, client, _, index, _ = system
    resident = app.state.service.snapshot().engine
    response = client.get("/search", params={"q": query, "explain": True})
    assert response.status_code == 200
    result = response.json()
    reference = json.loads(subprocess.run([str(BINARY), "search", query, "--index", str(index), "--json", "--explain"],
                                         check=True, capture_output=True, text=True).stdout)
    assert result["total"] == reference["total"]
    assert result["expanded_terms"] == reference["expanded_terms"]
    assert [hit["path"] for hit in result["hits"]] == [hit["path"] for hit in reference["hits"]]
    for actual, expected in zip(result["hits"], reference["hits"]):
        assert actual["score"] == pytest.approx(expected["score"], rel=1e-8)
        assert actual["snippet"] == expected["snippet"]
        assert sum(e["score"] for e in actual["explanation"]) == pytest.approx(actual["score"])
    assert app.state.service.snapshot().engine is resident


@pytest.mark.parametrize("params", [{}, {"q": ""}, {"q": "  "}, {"q": "memory OR"},
                                    {"q": "*"}, {"q": "memory", "k": 0}, {"q": "memory", "k": 1001},
                                    {"q": "memory", "k": "bad"}, {"q": "ก" * 1400}])
def test_invalid_requests(system, params):
    assert system[1].get("/search", params=params).status_code == 422


def test_contracts_and_no_shell(system, monkeypatch):
    _, client, _, _, _ = system
    monkeypatch.setattr(subprocess, "run", lambda *a, **k: pytest.fail("API must not spawn CLI per request"))
    assert client.get("/health").json()["status"] == "ok"
    assert client.get("/stats").json()["documents"] == 4
    response = client.get("/search", params={"q": "memory", "k": 1})
    assert response.status_code == 200
    assert response.json()["total"] == 2
    assert len(response.json()["hits"]) == 1
    assert "explanation" not in response.json()["hits"][0]
    assert client.get("/search", params={"q": "not_a_word"}).json()["hits"] == []
    paths = client.get("/openapi.json").json()["paths"]
    assert set(paths) == {"/search", "/stats", "/health", "/reload", "/collections"}


def test_reload_keeps_old_snapshot_until_commit(system):
    app, client, corpus, _, rebuild = system
    old = app.state.service.snapshot()
    (corpus / "new.md").write_text("memory newword")
    rebuild()
    assert client.get("/stats").json()["source_changed"] is True
    assert client.get("/search", params={"q": "newword"}).json()["total"] == 0
    response = client.post("/reload")
    assert response.status_code == 200
    assert response.json()["documents"] == 5
    assert response.json()["snapshot_id"] != old.version
    assert client.get("/search", params={"q": "newword"}).json()["total"] == 1
    assert old.engine.search("newword")["total"] == 0


def test_bad_reload_preserves_resident_data(system):
    app, client, _, index, rebuild = system
    before = app.state.service.snapshot()
    index.write_bytes(b"damaged snapshot")
    assert client.post("/reload").status_code == 409
    assert app.state.service.snapshot() is before
    assert client.get("/search", params={"q": "memory"}).json()["total"] == 2
    rebuild()
    assert client.post("/reload").status_code == 200


def test_parallel_search_during_reload(system):
    app, _, corpus, _, rebuild = system
    (corpus / "new.md").write_text("memory newword")
    rebuild()
    service = app.state.service
    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = [pool.submit(service.search, "memory", 10, True) for _ in range(30)]
        reload_job = pool.submit(service.reload)
        futures += [pool.submit(service.search, "memory", 10, True) for _ in range(30)]
        results = [f.result() for f in futures]
        reload_job.result()
    assert all(r["total"] in {2, 3} for r in results)
    assert service.search("memory", 10, True)["total"] == 3


@pytest.fixture
def ui(system, monkeypatch):
    import web.client
    client = system[1]

    def call(path, *, method="GET", params=None, files=None):
        response = client.request(method, path, params=params, files=files)
        if response.is_error:
            raise APIError(str(response.json()["detail"]))
        result = response.json()
        result["round_trip_ms"] = .5
        return result

    monkeypatch.setattr(web.client, "call_api", call)
    return AppTest.from_file(str(ROOT / "web/ui.py"), default_timeout=15).run()


def test_ui_search_explanation_and_filter(ui):
    assert not ui.exception
    ui.text_input(key="query").input('"memory allocation"')
    next(b for b in ui.button if b.label == "Search").click().run()
    assert not ui.exception
    assert ui.session_state["results"]["total"] == 1
    assert len(ui.dataframe) == 1
    ui.text_input(key="query").input("memory")
    next(s for s in ui.selectbox if s.label == "File type").select("C++ (.cpp)")
    next(b for b in ui.button if b.label == "Search").click().run()
    assert not ui.exception
    assert ui.session_state["results"]["hits"][0]["path"] == "b.cpp"


def test_ui_invalid_empty_and_examples(ui):
    ui.text_input(key="query").input("memory OR")
    next(b for b in ui.button if b.label == "Search").click().run()
    assert not ui.exception
    assert ui.error
    ui.text_input(key="query").input("")
    next(b for b in ui.button if b.label == "Search").click().run()
    assert ui.warning
    next(b for b in ui.button if b.label == "Try a typo").click().run()
    assert ui.text_input(key="query").value == "memry~1"
    next(b for b in ui.button if b.label == "Search").click().run()
    assert ui.session_state["results"]["total"] == 2


def test_ui_reload(ui):
    next(b for b in ui.button if b.label == "Search").click().run()
    next(b for b in ui.button if b.label == "Reload index").click().run()
    assert not ui.exception
    assert ui.success
    assert "results" not in ui.session_state


def test_ui_api_offline(monkeypatch):
    import web.client

    def offline(*args, **kwargs):
        raise APIError("Search API offline")

    monkeypatch.setattr(web.client, "call_api", offline)
    ui = AppTest.from_file(str(ROOT / "web/ui.py")).run()
    assert not ui.exception
    assert ui.error[0].value == "Search API offline"


def test_upload_search_isolated_and_utf8(system):
    app, client, _, _, _ = system
    default = app.state.service.snapshot()
    response = client.post("/collections", files=[
        ("files", ("notes.md", b"uploadonly memory allocation", "text/plain")),
        ("files", ("unicode.txt", "ไทย สวัสดี uploadonly".encode(), "text/plain")),
    ])
    assert response.status_code == 201, response.text
    collection = response.json()["collection_id"]
    assert response.json()["documents"] == 2
    assert client.get("/search", params={"q": "uploadonly"}).json()["total"] == 0
    assert client.get("/search", params={"q": "uploadonly", "collection": collection}).json()["total"] == 2
    assert client.get("/search", params={"q": "ไทย", "collection": collection}).json()["total"] == 1
    assert client.get("/stats", params={"collection": collection}).json()["documents"] == 2
    assert app.state.service.snapshot() is default


@pytest.mark.parametrize("files", [
    [("files", ("../escape.txt", b"text"))],
    [("files", ("folder\\escape.txt", b"text"))],
    [("files", ("report.pdf", b"%PDF"))],
    [("files", ("bad.txt", b"a\0b"))],
    [("files", ("bad.txt", b"\xff"))],
    [("files", ("same.txt", b"one")), ("files", ("SAME.txt", b"two"))],
])
def test_reject_invalid_uploads(system, files):
    app, client, _, _, _ = system
    response = client.post("/collections", files=files)
    assert response.status_code == 422, response.text
    assert not app.state.uploads._items
    assert client.get("/search", params={"q": "memory"}).json()["total"] == 2


def test_upload_bounds_and_expiration(system, monkeypatch):
    import web.api
    _, client, _, _, _ = system
    monkeypatch.setattr(web.api, "MAX_FILES", 1)
    assert client.post("/collections", files=[("files", ("a.txt", b"a")), ("files", ("b.txt", b"b"))]).status_code == 422
    monkeypatch.setattr(web.api, "MAX_FILE_BYTES", 3)
    assert client.post("/collections", files=[("files", ("big.txt", b"four"))]).status_code == 413
    monkeypatch.setattr(web.api, "MAX_FILE_BYTES", 100)
    monkeypatch.setattr(web.api, "MAX_COLLECTIONS", 1)
    first = client.post("/collections", files=[("files", ("first.txt", b"firstword"))]).json()["collection_id"]
    second = client.post("/collections", files=[("files", ("second.txt", b"secondword"))]).json()["collection_id"]
    assert client.get("/search", params={"q": "firstword", "collection": first}).status_code == 404
    assert client.get("/search", params={"q": "secondword", "collection": second}).json()["total"] == 1
    assert client.get("/stats", params={"collection": "../escape"}).status_code == 422


def test_upload_total_and_request_body_bound(system, monkeypatch):
    import web.api
    client = system[1]
    monkeypatch.setattr(web.api, "MAX_BATCH_BYTES", 3)
    assert client.post("/collections", files=[("files", ("a.txt", b"four"))]).status_code == 413
    response = client.post("/collections", content=b"x" * (1024 * 1024 + 10),
                           headers={"Content-Type": "multipart/form-data; boundary=x"})
    assert response.status_code == 413


def test_upload_cleanup_on_shutdown(tmp_path):
    corpus = tmp_path / "corpus"
    corpus.mkdir()
    (corpus / "a.txt").write_text("example")
    index = tmp_path / "snapshot.lsi"
    subprocess.run([str(BINARY), "index", str(corpus), "--index", str(index)], check=True, capture_output=True)
    with TestClient(create_app(index)) as client:
        response = client.post("/collections", files=[("files", ("upload.txt", b"temporary"))])
        root = Path(response.json()["root"])
        assert root.exists()
    assert not root.exists()


def test_ui_upload_search_and_switch_collection(ui, monkeypatch):
    import io
    import streamlit
    file = io.BytesIO(b"onlyuploaded uniquephrase")
    file.name = "uploaded.txt"
    file.size = len(file.getvalue())
    monkeypatch.setattr(streamlit, "file_uploader", lambda *args, **kwargs: [file])
    ui.run()
    next(b for b in ui.button if b.label == "Index uploaded files").click().run()
    assert not ui.exception
    assert ui.session_state["uploaded_collection"]
    assert ui.session_state["collection_choice"] == "Uploaded files"
    assert ui.success
    ui.text_input(key="query").input("onlyuploaded")
    next(b for b in ui.button if b.label == "Search").click().run()
    assert ui.session_state["results"]["total"] == 1
    assert ui.session_state["results"]["hits"][0]["path"] == "uploaded.txt"
    ui.radio(key="collection_choice").set_value("Default collection").run()
    assert "results" not in ui.session_state
    next(b for b in ui.button if b.label == "Search").click().run()
    assert ui.session_state["results"]["total"] == 0
