# Information retrieval

An inverted index maps each term to a sorted posting list. Positional postings
support phrase queries. BM25 uses term frequency, inverse document frequency,
and document length normalization to rank search results.

A BK-tree prunes fuzzy search using the triangle inequality of edit distance.
