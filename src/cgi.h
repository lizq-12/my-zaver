#ifndef ZV_CGI_H
#define ZV_CGI_H

#include "http_request.h"

/* Start CGI for a request (GET-only MVP). Returns 0 on success, -1 on failure. */
int zv_cgi_start(zv_http_request_t *r, const char *script_filename, const char *script_name, const char *query_string);

/* Called when CGI stdout pipe is readable. */
void zv_cgi_on_stdout_ready(zv_http_request_t *r);

/* Called on client socket EPOLLOUT while CGI is active. */
int zv_cgi_on_client_writable(zv_http_request_t *r);

#endif
