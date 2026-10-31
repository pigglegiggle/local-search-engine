# Architecture diagrams

The README uses three separate text-first diagrams in the same style as the TCP Load Balancer project: white rectangular nodes, black text, and simple connectors. There are no icons, gradients or decorative illustrations. The existing web UI screenshot remains a separate example of the interface.

| Topic | README image | Editable source |
|---|---|---|
| Parallel analysis, deterministic merge and snapshot persistence | [Indexing](images/architecture-indexing.svg) | [draw.io source](images/architecture-indexing.drawio) |
| Resident retrieval, ranking and bounded top-k selection | [Search](images/architecture-search.svg) | [draw.io source](images/architecture-search.drawio) |
| CLI/REPL and Streamlit → FastAPI → native engine | [Interfaces](images/architecture-interfaces.svg) | [draw.io source](images/architecture-interfaces.drawio) |

Open `.drawio` files in diagrams.net or the draw.io desktop application to edit individual boxes, labels and connectors. Export the edited drawing as SVG and replace its matching README image.

The indexing diagram uses dashed arrows for reuse from the previous snapshot. Unchanged postings are remapped to the new document IDs during merge; the engine still reads source bytes and writes a complete replacement snapshot. Its previous-snapshot node represents the logical input, not a late load after file analysis.

The search diagram shows logical responsibilities around a reusable resident index. Parsing and evaluation are interleaved in the implementation; no separate expression-tree structure is built. Boolean evaluation and positive-term BM25 scoring happen before bounded top-k selection. The heap bounds selected results, not all intermediate query memory. Prefix lookup uses the ordered dictionary; fuzzy expansion uses the BK-tree.

The interface diagram shows call paths, not separate deployments for every box. pybind11 loads the C++ implementation into the API process; it is not a network service. Upload indexing also uses that native implementation and produces isolated temporary collections.

The final assets are native SVG and editable draw.io XML. They replace the former AI-generated raster illustrations. See [ARCHITECTURE.md](ARCHITECTURE.md) for implementation details and correctness invariants.
