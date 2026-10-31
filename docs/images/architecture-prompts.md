# Architecture image prompts

Generated with the built-in imagegen tool as three separate black-and-white diagrams. Names and flows were checked against `src/`, `bindings/python.cpp`, `web/api.py` and `docs/ARCHITECTURE.md`. See the architecture document for the detailed implementation.

## architecture-indexing.png

```text
Use case: infographic-diagram. Asset: ONE standalone architecture diagram for a GitHub README. Generate only the specified single diagram, never a collage or a multi-section overview. Landscape aspect ratio about 2.4:1, ideally 1920x800. Strict BLACK AND WHITE: solid pure white background; black text, black outlines, black connectors; boxes have white interiors. No blue, navy, colored accents, tinted panels, gradients, shadows, texture or decoration. Clean technical system-design notation, precise geometry, generous whitespace, modern highly legible sans-serif typography. Large labels readable at 900px display width. All text English and exact. Clear black arrowheads touch the destination component. Avoid line crossings. Do not add components, databases, cloud services, icons, performance numbers, promotional claims, extra sections or footers.
Title "Indexing pipeline". Subtitle "Local files → persistent snapshot".
Show a SINGLE horizontal left-to-right chain of four white, black-outlined boxes:
"Local UTF-8 files" with subtitle "Text + source code"
→ "Parallel indexing" with subtitle "Read · tokenize · merge"
→ "Positional inverted index" with subtitle "Term → document IDs → positions"
→ "Snapshot (.lsi)" with subtitle "Delta / varint · CRC32".
Under the final box a small annotation: "fsync + atomic rename".
Below the chain, one readable note: "Incremental updates reuse unchanged postings; each update writes a complete snapshot."
This image covers indexing ONLY. Do not include query search, user interfaces or resident indexes.
```

## architecture-search.png

```text
Use case: infographic-diagram. Asset: ONE standalone architecture diagram for a GitHub README. Generate only the specified single diagram, never a collage or a multi-section overview. Landscape aspect ratio about 2.4:1, ideally 1920x800. Strict BLACK AND WHITE: solid pure white background; black text, black outlines, black connectors; boxes have white interiors. No blue, navy, colored accents, tinted panels, gradients, shadows, texture or decoration. Clean technical system-design notation, precise geometry, generous whitespace, modern highly legible sans-serif typography. Large labels readable at 900px display width. All text English and exact. Clear black arrowheads touch the destination component. Avoid line crossings. Do not add components, databases, cloud services, icons, performance numbers, promotional claims, extra sections or footers.
Title "Search pipeline". Subtitle "Query execution inside the C++17 engine".
Show the main horizontal pipeline left-to-right:
small "Query" input
→ "Query parser" with subtitle "NOT > AND > OR"
→ "Retrieve + score" with subtitle "Boolean · phrases · BM25"
→ "Top-k + snippets" with subtitle "Bounded heap"
→ "Ranked results" with subtitle "Paths · scores · snippets".
Above the main pipeline show TWO smaller boxes connected LEFT TO RIGHT:
"Snapshot (.lsi)" → [arrow label "Load + validate"] → "Resident index".
Inside Resident index include two short sublabels, on separate lines: "Positional postings" and "BK-tree + prefix lookup".
Place Resident index directly above Retrieve + score. Draw ONE short vertical arrow from the BOTTOM EDGE of Resident index to the TOP EDGE of Retrieve + score. No other arrows from Resident index and no enclosing rectangles. Make the connections to these actual boxes unambiguous, not ending in whitespace.
Below the diagram, one note: "The CLI loads per command; REPL and API reuse a loaded index."
This image covers SEARCH ONLY, not the full file-indexing process or web components.
```

## architecture-interfaces.png

```text
Use case: infographic-diagram. Asset: ONE standalone architecture diagram for a GitHub README. Generate only the specified single diagram, never a collage or a multi-section overview. Landscape aspect ratio about 2.4:1, ideally 1920x800. Strict BLACK AND WHITE: solid pure white background; black text, black outlines, black connectors; boxes have white interiors. No blue, navy, colored accents, tinted panels, gradients, shadows, texture or decoration. Clean technical system-design notation, precise geometry, generous whitespace, modern highly legible sans-serif typography. Large labels readable at 900px display width. All text English and exact. Clear black arrowheads touch the destination component. Avoid line crossings. Do not add components, databases, cloud services, icons, performance numbers, promotional claims, extra sections or footers.
Title "CLI and web interfaces". Subtitle "Both paths use the same C++17 search engine".
Show TWO separate horizontal interface routes with generous spacing and no shared outer container:
Top route, labeled "Command line": "CLI / REPL" → "C++17 engine".
Bottom route, labeled "Web": "Streamlit UI" → [arrow label "HTTP"] → "FastAPI" → "pybind11" → "Resident C++17 engine".
Align and space the bottom route so each arrow clearly joins two boxes. Keep all components in the same left-to-right order.
Under the top route, note: "REPL keeps the index loaded between queries."
Under the bottom route, note: "The API holds immutable snapshots and supports safe reload."
At the bottom, a separate short note: "File uploads create isolated temporary collections."
This image covers interfaces ONLY. No indexing pipeline, search internals, or snapshot storage diagrams.
```
