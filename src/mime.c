/*
 * mime.c: tabela de tipos MIME mais comuns num site estático.
 */
#include "mime.h"

#include <ctype.h>
#include <string.h>

static const struct {
    const char *ext;
    const char *type;
} TABLE[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm", "text/html; charset=utf-8" },
    { "css", "text/css; charset=utf-8" },
    { "js", "text/javascript; charset=utf-8" },
    { "mjs", "text/javascript; charset=utf-8" },
    { "json", "application/json" },
    { "map", "application/json" },
    { "webmanifest", "application/manifest+json" },
    { "xml", "application/xml" },
    { "txt", "text/plain; charset=utf-8" },
    { "md", "text/markdown; charset=utf-8" },
    { "csv", "text/csv; charset=utf-8" },
    { "svg", "image/svg+xml" },
    { "png", "image/png" },
    { "jpg", "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif", "image/gif" },
    { "webp", "image/webp" },
    { "avif", "image/avif" },
    { "ico", "image/x-icon" },
    { "bmp", "image/bmp" },
    { "woff", "font/woff" },
    { "woff2", "font/woff2" },
    { "ttf", "font/ttf" },
    { "otf", "font/otf" },
    { "pdf", "application/pdf" },
    { "zip", "application/zip" },
    { "gz", "application/gzip" },
    { "tar", "application/x-tar" },
    { "wasm", "application/wasm" },
    { "mp3", "audio/mpeg" },
    { "wav", "audio/wav" },
    { "ogg", "audio/ogg" },
    { "oga", "audio/ogg" },
    { "m4a", "audio/mp4" },
    { "flac", "audio/flac" },
    { "mp4", "video/mp4" },
    { "m4v", "video/mp4" },
    { "webm", "video/webm" },
    { "ogv", "video/ogg" },
    { "mov", "video/quicktime" },
    { "vtt", "text/vtt; charset=utf-8" },
};

const char *mime_type(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || !dot[1]) return "application/octet-stream";
    char ext[16];
    size_t n = strlen(dot + 1);
    if (n >= sizeof ext) return "application/octet-stream";
    for (size_t i = 0; i <= n; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (strcmp(ext, TABLE[i].ext) == 0) return TABLE[i].type;
    }
    return "application/octet-stream";
}
