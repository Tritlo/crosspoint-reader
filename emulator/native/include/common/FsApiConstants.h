#pragma once

#include <cstdint>

using oflag_t = uint16_t;

constexpr oflag_t O_RDONLY = 0x0000;
constexpr oflag_t O_WRONLY = 0x0001;
constexpr oflag_t O_RDWR = 0x0002;
constexpr oflag_t O_APPEND = 0x0004;
constexpr oflag_t O_AT_END = 0x0008;
constexpr oflag_t O_CREAT = 0x0010;
constexpr oflag_t O_TRUNC = 0x0020;
constexpr oflag_t O_EXCL = 0x0040;
constexpr oflag_t O_SYNC = 0x0080;
constexpr oflag_t O_WRITE = O_WRONLY;
