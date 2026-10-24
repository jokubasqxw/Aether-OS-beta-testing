#ifndef KERNEL_TLS_HPP
#define KERNEL_TLS_HPP

#include "types.hpp"

namespace tls {

// Fetch an HTTPS URL. The returned data is a complete HTTP/1.1 response
// (headers + body), suitable for the browser's existing parser.
int https_get(const uint8_t server_ip[4], uint16_t port, const char* host,
              const char* path, char* response, size_t max_len,
              uint32_t timeout_ms = 15000);

const char* last_error();

} // namespace tls

#endif // KERNEL_TLS_HPP
