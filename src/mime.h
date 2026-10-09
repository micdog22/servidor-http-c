/*
 * mime.h: tipo de conteúdo pela extensão do arquivo.
 */
#ifndef MIME_H
#define MIME_H

/* "foto.PNG" vira "image/png"; sem extensão conhecida, application/octet-stream. */
const char *mime_type(const char *path);

#endif
