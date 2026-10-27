/*
HOST_HTTPS.C

HTTPS for the guest (host_https_request: port/android/guest/runtime/
guest_host.h; guest_https.c makes update_https_request of it): the server
browser's lists of the master servers (port/linux/src/master_list.c), a
dedicated server's publishing to them (server_master.c), made in the host
with the desktop's HTTPS client (port/linux/src/posix_update.c's
update_https_request, Mbed TLS, the system's certificate authorities; on
Windows win32_update.c's, WinHTTP): the guest has no TLS of its own, and
its memory is the host's to read and write. The request crosses as text,
so that it has no structure and no more than six arguments (x32 passes
those after the sixth on the stack).
*/

#include "host.h"
#include "../../linux/src/update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (guest_host.h's) */
#define HOST_HTTPS_HEADERS_SIZE 4096

int host_https_request(const char *request, const void *body, int body_size, char *response, int response_size,
	int *results)
{
	struct update_response answer;
	char method[16], url[2048];
	const char *line = strchr(request, '\n');
	const char *headers = line ? strchr(line + 1, '\n') : NULL;
	long maximum = (long)response_size - HOST_HTTPS_HEADERS_SIZE - 1;

	results[0] = results[1] = 0;
	if (!line || !headers || line - request >= (long)sizeof(method) || headers - line - 1 >= (long)sizeof(url) ||
		maximum < 0)
	{
		snprintf(response, (size_t)response_size, "a request the host cannot read");
		return 0;
	}
	snprintf(method, sizeof(method), "%.*s", (int)(line - request), request);
	snprintf(url, sizeof(url), "%.*s", (int)(headers - line - 1), line + 1);
	headers++;
	if (!update_https_request(method, url, headers[0] ? headers : NULL, body_size > 0 ? body : NULL,
		body_size > 0 ? (unsigned long)body_size : 0, (unsigned long)maximum, &answer, response, response_size))
	{
		return 0;
	}
	results[0] = answer.status;
	snprintf(response, HOST_HTTPS_HEADERS_SIZE, "%s", answer.headers);
	if (answer.body_size > (unsigned long)maximum)
		answer.body_size = (unsigned long)maximum;
	if (answer.body)
		memcpy(response + HOST_HTTPS_HEADERS_SIZE, answer.body, answer.body_size);
	response[HOST_HTTPS_HEADERS_SIZE + answer.body_size] = 0;
	results[1] = (int)answer.body_size;
	free(answer.body);
	return 1;
}
