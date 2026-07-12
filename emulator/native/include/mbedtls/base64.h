#pragma once

#include <cstddef>

constexpr int MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL = -0x002A;

int mbedtls_base64_decode(unsigned char* destination, size_t destinationSize, size_t* written,
                          const unsigned char* source, size_t sourceSize);
