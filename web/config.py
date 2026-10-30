"""File formats and upload bounds shared by the API and UI."""
TEXT_EXTENSIONS = ["txt", "md", "markdown", "cpp", "hpp", "cc", "c", "h", "py", "rs", "go",
                   "java", "json", "yaml", "yml", "toml", "csv", "log", "sh", "sql", "js",
                   "ts", "tsx", "jsx", "html", "css", "rst"]
MAX_FILES = 50
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_BATCH_BYTES = 32 * 1024 * 1024
MAX_COLLECTIONS = 4
