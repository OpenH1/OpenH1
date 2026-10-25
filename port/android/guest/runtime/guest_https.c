/*
GUEST_HTTPS.C

update_https_request (port/linux/src/update.h) for the guest: made in the
host (host_https_request: port/android/host/host_https.c), the server
browser's requests to the master servers and a dedicated server's to them.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_host.h"
#include "update.h"

int update_https_request(const char *method, const char *url, const char *headers, const void *body,
	unsigned long body_size, unsigned long maximum, struct update_response *response, char *error, int error_size)
{
	size_t request_size = strlen(method) + strlen(url) + (headers ? strlen(headers) : 0) + 3;
	char *request;
	char *answer;
	unsigned long answer_size;
	int results[2] = { 0, 0 };
	int answered;

	memset(response, 0, sizeof(*response));
	/* (an answer within what 32 bits address, and the guest's memory has) */
	if (maximum > 64UL * 1024 * 1024 || body_size > 64UL * 1024 * 1024)
	{
		snprintf(error, (size_t)error_size, "a request too large");
		return 0;
	}
	answer_size = HOST_HTTPS_HEADERS_SIZE + maximum + 1;
	request = malloc(request_size);
	answer = malloc(answer_size);
	if (!request || !answer)
	{
		free(request);
		free(answer);
		snprintf(error, (size_t)error_size, "out of memory");
		return 0;
	}
	snprintf(request, request_size, "%s\n%s\n%s", method, url, headers ? headers : "");
	answered = host_https_request(request, body, (int)body_size, answer, (int)answer_size, results);
	free(request);
	if (!answered)
	{
		snprintf(error, (size_t)error_size, "%s", answer);
		free(answer);
		return 0;
	}
	response->status = results[0];
	snprintf(response->headers, sizeof(response->headers), "%s", answer);
	response->body_size = (unsigned long)results[1];
	response->body = malloc(response->body_size + 1);
	if (!response->body)
	{
		free(answer);
		snprintf(error, (size_t)error_size, "out of memory");
		return 0;
	}
	memcpy(response->body, answer + HOST_HTTPS_HEADERS_SIZE, response->body_size);
	response->body[response->body_size] = 0;
	free(answer);
	return 1;
}
