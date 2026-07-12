#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

class __FlashStringHelper;
#define FPSTR(pointer) (reinterpret_cast<const __FlashStringHelper*>(pointer))
#define F(literal) (FPSTR(literal))

class String : public std::string {
 public:
  using std::string::operator=;

  String() = default;
  String(const char* value) : std::string(value == nullptr ? "" : value) {}
  String(const char* value, unsigned int length)
      : std::string(value == nullptr ? "" : value, value == nullptr ? 0 : length) {}
  String(const uint8_t* value, unsigned int length)
      : std::string(value == nullptr ? "" : reinterpret_cast<const char*>(value), length) {}
  String(const std::string& value) : std::string(value) {}
  String(std::string&& value) : std::string(std::move(value)) {}
  String(const __FlashStringHelper* value) : String(reinterpret_cast<const char*>(value)) {}
  explicit String(char value) : std::string(1, value) {}
  explicit String(unsigned char value, unsigned char base = 10) : String(number(value, base)) {}
  explicit String(int value, unsigned char base = 10) : String(number(value, base)) {}
  explicit String(unsigned int value, unsigned char base = 10) : String(number(value, base)) {}
  explicit String(long value, unsigned char base = 10) : String(number(value, base)) {}
  explicit String(unsigned long value, unsigned char base = 10) : String(number(value, base)) {}
  explicit String(long long value, unsigned char base = 10) : String(number(value, base)) {}
  explicit String(unsigned long long value, unsigned char base = 10) : String(number(value, base)) {}
  explicit String(float value, unsigned int decimals = 2) : String(decimal(value, decimals)) {}
  explicit String(double value, unsigned int decimals = 2) : String(decimal(value, decimals)) {}

  bool reserve(unsigned int size) {
    std::string::reserve(size);
    return capacity() >= size;
  }
  unsigned int length() const { return static_cast<unsigned int>(size()); }
  bool isEmpty() const { return empty(); }
  explicit operator bool() const { return true; }

  bool concat(const String& value) { return concat(value.data(), value.size()); }
  bool concat(const char* value) { return value != nullptr && concat(value, std::strlen(value)); }
  bool concat(const char* value, unsigned int length) {
    if (value == nullptr) return false;
    append(value, length);
    return true;
  }
  bool concat(char value) {
    push_back(value);
    return true;
  }
  bool concat(unsigned char value) { return concat(String(value)); }
  bool concat(int value) { return concat(String(value)); }
  bool concat(unsigned int value) { return concat(String(value)); }
  bool concat(long value) { return concat(String(value)); }
  bool concat(unsigned long value) { return concat(String(value)); }
  bool concat(long long value) { return concat(String(value)); }
  bool concat(unsigned long long value) { return concat(String(value)); }
  bool concat(float value) { return concat(String(value)); }
  bool concat(double value) { return concat(String(value)); }

  String& append(const char* value) {
    std::string::append(value == nullptr ? "" : value);
    return *this;
  }
  String& append(const char* value, size_t count) {
    if (value != nullptr) std::string::append(value, count);
    return *this;
  }

  String& operator+=(char value) {
    push_back(value);
    return *this;
  }
  String& operator+=(unsigned char value) { return appendNumber(value); }
  String& operator+=(int value) { return appendNumber(value); }
  String& operator+=(unsigned int value) { return appendNumber(value); }
  String& operator+=(long value) { return appendNumber(value); }
  String& operator+=(unsigned long value) { return appendNumber(value); }
  String& operator+=(long long value) { return appendNumber(value); }
  String& operator+=(unsigned long long value) { return appendNumber(value); }
  String& operator+=(float value) { return appendNumber(value); }
  String& operator+=(double value) { return appendNumber(value); }
  using std::string::operator+=;

  int compareTo(const String& value) const { return compare(value); }
  bool equals(const String& value) const { return *this == value; }
  bool equals(const char* value) const { return value != nullptr && *this == value; }
  bool equalsIgnoreCase(const String& value) const {
    return size() == value.size() && std::equal(begin(), end(), value.begin(), [](char left, char right) {
             return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
           });
  }
  bool startsWith(const String& prefix) const { return starts_with(prefix); }
  bool startsWith(const char* prefix) const { return prefix != nullptr && starts_with(prefix); }
  bool startsWith(const String& prefix, unsigned int offset) const {
    return offset <= size() && compare(offset, prefix.size(), prefix) == 0;
  }
  bool endsWith(const String& suffix) const { return ends_with(suffix); }
  bool endsWith(const char* suffix) const { return suffix != nullptr && ends_with(suffix); }

  char charAt(unsigned int index) const { return index < size() ? (*this)[index] : 0; }
  void setCharAt(unsigned int index, char value) {
    if (index < size()) (*this)[index] = value;
  }
  void getBytes(unsigned char* buffer, unsigned int bufferSize, unsigned int index = 0) const {
    if (buffer == nullptr || bufferSize == 0) return;
    const size_t count = index < size() ? std::min<size_t>(bufferSize - 1, size() - index) : 0;
    if (count > 0) std::memcpy(buffer, data() + index, count);
    buffer[count] = 0;
  }
  void toCharArray(char* buffer, unsigned int bufferSize, unsigned int index = 0) const {
    getBytes(reinterpret_cast<unsigned char*>(buffer), bufferSize, index);
  }

  int indexOf(char value, unsigned int fromIndex = 0) const { return position(find(value, fromIndex)); }
  int indexOf(const String& value, unsigned int fromIndex = 0) const { return position(find(value, fromIndex)); }
  int lastIndexOf(char value) const { return position(rfind(value)); }
  int lastIndexOf(char value, unsigned int fromIndex) const { return position(rfind(value, fromIndex)); }
  int lastIndexOf(const String& value) const { return position(rfind(value)); }
  int lastIndexOf(const String& value, unsigned int fromIndex) const { return position(rfind(value, fromIndex)); }

  String substring(unsigned int beginIndex) const { return substring(beginIndex, size()); }
  String substring(unsigned int beginIndex, unsigned int endIndex) const {
    if (beginIndex > endIndex) std::swap(beginIndex, endIndex);
    beginIndex = std::min<unsigned int>(beginIndex, size());
    endIndex = std::min<unsigned int>(endIndex, size());
    return substr(beginIndex, endIndex - beginIndex);
  }
  void replace(char findValue, char replacement) { std::replace(begin(), end(), findValue, replacement); }
  void replace(const String& findValue, const String& replacement) {
    if (findValue.empty()) return;
    size_t offset = 0;
    while ((offset = find(findValue, offset)) != npos) {
      std::string::replace(offset, findValue.size(), replacement);
      offset += replacement.size();
    }
  }
  void remove(unsigned int index) { remove(index, size() - std::min<size_t>(index, size())); }
  void remove(unsigned int index, unsigned int count) {
    if (index < size()) erase(index, count);
  }
  void toLowerCase() {
    std::transform(begin(), end(), begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  }
  void toUpperCase() {
    std::transform(begin(), end(), begin(), [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
  }
  void trim() {
    const auto nonSpace = [](unsigned char value) { return !std::isspace(value); };
    const auto first = std::find_if(begin(), end(), nonSpace);
    const auto last = std::find_if(rbegin(), rend(), nonSpace).base();
    *this = first < last ? String(std::string(first, last)) : String();
  }

  long toInt() const { return std::strtol(c_str(), nullptr, 10); }
  float toFloat() const { return std::strtof(c_str(), nullptr); }
  double toDouble() const { return std::strtod(c_str(), nullptr); }

 private:
  template <typename Number>
  static std::string number(Number value, unsigned char base) {
    if (base < 2 || base > 36) base = 10;
    using Unsigned = std::make_unsigned_t<Number>;
    const bool negative = std::is_signed_v<Number> && value < 0;
    Unsigned remaining = negative ? Unsigned(0) - static_cast<Unsigned>(value) : static_cast<Unsigned>(value);
    std::string result;
    do {
      const unsigned int digit = remaining % base;
      result.push_back(static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10));
      remaining /= base;
    } while (remaining != 0);
    if (negative) result.push_back('-');
    std::reverse(result.begin(), result.end());
    return result;
  }

  static std::string decimal(double value, unsigned int decimals);

  template <typename Number>
  String& appendNumber(Number value) {
    std::string::append(String(value));
    return *this;
  }

  static int position(size_t value) { return value == npos ? -1 : static_cast<int>(value); }
};

using StringSumHelper = String;
