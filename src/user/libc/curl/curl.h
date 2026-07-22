#ifndef BUZZOS_CURL_TYPE_STUB_H
#define BUZZOS_CURL_TYPE_STUB_H

/* NetSurf's fetch factory exposes the CURLM pointer even when WITH_CURL is
 * disabled. BuzzOS supplies its own native HTTP fetcher, so only the opaque
 * type is needed here. */
typedef struct CURLM CURLM;

#endif
