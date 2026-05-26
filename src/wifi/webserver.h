#ifndef WIFI_WEBSERVER_H
#define WIFI_WEBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEBSERVER_BODY_MAX_LEN 4096

typedef struct {
   const char *method;
   const char *path;
   const uint8_t *body;
   size_t body_length;
} webserver_request_t;

typedef struct {
   uint16_t status_code;
   const char *content_type;
   char body[WEBSERVER_BODY_MAX_LEN];
   size_t body_length;
} webserver_response_t;

void webserver_init(void);
bool webserver_is_ready(void);
const char *webserver_last_error(void);
bool webserver_render(const webserver_request_t *request, webserver_response_t *response);

#endif