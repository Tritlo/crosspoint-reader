#pragma once

#include <string_view>

enum class ActivityId { Unknown, Boot, Home, FileBrowser, ReaderEpub };

constexpr std::string_view activityIdName(ActivityId id) {
  switch (id) {
    case ActivityId::Boot:
      return "boot";
    case ActivityId::Home:
      return "home";
    case ActivityId::FileBrowser:
      return "file_browser";
    case ActivityId::ReaderEpub:
      return "reader.epub";
    case ActivityId::Unknown:
      return "";
  }
  return "";
}
