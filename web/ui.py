"""A default Streamlit interface for uploading and searching local text files."""
from pathlib import Path
import sys

import streamlit as st

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from web.client import APIError, call_api
from web.config import MAX_BATCH_BYTES, MAX_FILES, TEXT_EXTENSIONS

st.set_page_config(page_title="Local Search", layout="centered")
st.title("Local Search")
st.write("Upload text files or search the default collection.")


def choose_example(query: str):
    st.session_state["query"] = query


uploaded_id = st.session_state.get("uploaded_collection")
use_uploaded = uploaded_id and st.session_state.get("collection_choice") == "Uploaded files"
collection_params = {"collection": uploaded_id} if use_uploaded else {}
try:
    stats = call_api("/stats", params=collection_params)
except APIError as error:
    st.error(str(error))
    if use_uploaded:
        if st.button("Use default collection"):
            st.session_state["collection_choice"] = "Default collection"
            st.session_state.pop("results", None)
            st.rerun()
    else:
        st.code("make api", language="bash")
        st.button("Retry connection")
    st.stop()

st.subheader("Upload files")
files = st.file_uploader(
    "Choose text or source files", type=TEXT_EXTENSIONS, accept_multiple_files=True,
    max_upload_size=8, key="upload_files",
    help="UTF-8 text and source code. Up to 50 files, 8 MiB each, 32 MiB total. PDF and Word are not supported.",
)
st.caption("Uploads create a separate temporary collection. Original files and the default collection stay unchanged.")
if st.button("Index uploaded files", disabled=not files):
    if len(files) > MAX_FILES or sum(file.size for file in files) > MAX_BATCH_BYTES:
        st.error("Upload at most 50 files and 32 MiB in total.")
    else:
        try:
            with st.spinner("Indexing uploaded files"):
                result = call_api("/collections", method="POST", files=[
                    ("files", (file.name, file.getvalue(), "application/octet-stream")) for file in files
                ])
            st.session_state["uploaded_collection"] = result["collection_id"]
            st.session_state["collection_choice"] = "Uploaded files"
            st.session_state.pop("results", None)
            st.session_state["notice"] = f"Indexed {result['documents']} uploaded files. Ready to search."
            st.rerun()
        except APIError as error:
            st.error(str(error))

with st.sidebar:
    st.header("Collection")
    if uploaded_id:
        st.radio("Search in", ["Default collection", "Uploaded files"], key="collection_choice",
                 on_change=lambda: st.session_state.pop("results", None))
    st.write("Uploaded files" if use_uploaded else "Default collection")
    st.write(f"**Files:** {stats['documents']:,}")
    st.write(f"**Unique terms:** {stats['terms']:,}")
    if not use_uploaded:
        with st.expander("Folder"):
            st.code(stats["root"], language=None, wrap_lines=True)
        if stats["source_changed"]:
            st.warning("The index file has changed. Reload to use the latest snapshot.")
        if st.button("Reload index"):
            try:
                call_api("/reload", method="POST")
                st.session_state.pop("results", None)
                st.session_state["notice"] = "Index reloaded. Ready to search."
                st.rerun()
            except APIError as error:
                st.error(str(error))
        st.caption("Rebuild the index first if source files have changed.")
    else:
        st.caption("Temporary uploads are removed when the API stops. Older collections may expire after more uploads.")

if notice := st.session_state.pop("notice", None):
    st.success(notice)

st.subheader("Search")
with st.form("search", border=False):
    st.text_input("Search query", value='"memory allocation"', key="query",
                  placeholder="Enter a word or phrase")
    left, right = st.columns(2)
    limit = left.selectbox("Result limit", [10, 20, 50, 100])
    extension = right.selectbox("File type", ["All files", "Markdown (.md)", "C++ (.cpp)", "Python (.py)", "Text (.txt)"])
    explain = st.checkbox("Include BM25 explanations", value=True)
    submitted = st.form_submit_button("Search", type="primary")

with st.expander("Search examples"):
    st.caption('Use quotes for a phrase, * for a prefix, and ~1 for typo tolerance.')
    for label, query in [("Try a phrase", '"memory allocation"'), ("Try a typo", "memry~1"),
                         ("Try a Boolean query", "(cache OR memory) NOT ext:cpp")]:
        st.button(label, on_click=choose_example, args=(query,))

if submitted:
    query = st.session_state["query"].strip()
    st.session_state.pop("results", None)
    if not query:
        st.warning("Enter a query before searching.")
    else:
        if extension != "All files":
            ext = extension.split("(.")[1].rstrip(")")
            query = f"({query}) AND ext:{ext}"
        try:
            with st.spinner("Searching"):
                st.session_state["results"] = call_api("/search", params={
                    "q": query, "k": limit, "explain": explain, **collection_params,
                })
        except APIError as error:
            st.error(str(error))

result = st.session_state.get("results")
if result is not None:
    st.subheader("Results")
    st.caption(f"Query: {result['query']}")
    if result["snapshot_id"] != stats["snapshot_id"]:
        st.info("These results use an older snapshot. Search again to use the current index.")
    matches = "matching file" if result["total"] == 1 else "matching files"
    st.write(f"**{result['total']} {matches}** · C++ search: {result['search_ms']:.3f} ms · API round trip: {result['round_trip_ms']:.1f} ms")
    if not result["hits"]:
        st.info("No matching files. Try fewer conditions, a prefix such as sched*, or typo tolerance such as memry~1.")
    else:
        for rank, hit in enumerate(result["hits"], 1):
            st.markdown(f"#### Result {rank}")
            st.text(hit["path"])
            st.caption(f"BM25 score: {hit['score']:.4f}")
            st.text(hit["snippet"] or "No text is available for a preview.")
            if hit.get("explanation"):
                with st.expander("BM25 explanation"):
                    st.caption("tf: occurrences in this file. df: files containing the term.")
                    st.dataframe([{"Term": e["term"], "tf": e["tf"], "df": e["df"], "Contribution": e["score"]}
                                  for e in hit["explanation"]], hide_index=True)
            st.divider()
