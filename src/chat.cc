#include "chat.h"

#include <algorithm>

namespace nac {
namespace {

std::vector<std::string> SplitUtf8(const std::string& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = s[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    len = std::min(len, s.size() - i);
    out.push_back(s.substr(i, len));
    i += len;
  }
  return out;
}

}  // namespace

void ChatTracker::Open(bool secret, bool multiline) {
  active_ = true;
  secret_ = secret;
  multiline_ = multiline;
  last_length_ = 0;
  Clear();
}

void ChatTracker::Close() {
  active_ = false;
  Clear();
}

void ChatTracker::Clear() {
  chars_.clear();
  cursor_ = 0;
  selected_all_ = false;
}

void ChatTracker::Insert(const std::vector<std::string>& chars) {
  if (selected_all_) Clear();  // typing replaces the selection
  chars_.insert(chars_.begin() + cursor_, chars.begin(), chars.end());
  cursor_ += chars.size();
}

void ChatTracker::Text(const std::string& utf8) {
  if (active_ && !secret_) Insert(SplitUtf8(utf8));
}

void ChatTracker::Paste(const std::string& utf8) {
  if (!active_ || secret_) return;
  // Pasted line breaks become single spaces.
  std::string flat;
  bool space = false;
  for (char c : utf8) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      space = !flat.empty();
    } else {
      if (space) flat += ' ';
      space = false;
      flat += c;
    }
  }
  Insert(SplitUtf8(flat));
}

size_t ChatTracker::WordLeft() const {
  size_t i = cursor_;
  while (i > 0 && chars_[i - 1] == " ") --i;
  while (i > 0 && chars_[i - 1] != " ") --i;
  return i;
}

size_t ChatTracker::WordRight() const {
  size_t i = cursor_;
  while (i < chars_.size() && chars_[i] == " ") ++i;
  while (i < chars_.size() && chars_[i] != " ") ++i;
  return i;
}

std::optional<std::string> ChatTracker::Key(ChatKey key, bool ctrl) {
  if (!active_ || secret_) return std::nullopt;
  if (selected_all_) {
    // A selection of everything: deleting empties the box, moving drops it.
    switch (key) {
      case ChatKey::kBackspace:
      case ChatKey::kDelete:
        Clear();
        return std::nullopt;
      case ChatKey::kLeft:
      case ChatKey::kHome:
        selected_all_ = false;
        cursor_ = 0;
        return std::nullopt;
      case ChatKey::kRight:
      case ChatKey::kEnd:
        selected_all_ = false;
        cursor_ = chars_.size();
        return std::nullopt;
      default:
        break;
    }
  }
  switch (key) {
    case ChatKey::kReturn: {
      if (multiline_) {
        Insert({"\n"});
        return std::nullopt;
      }
      std::string message;
      for (const auto& c : chars_) message += c;
      last_length_ = chars_.size();
      Clear();
      const size_t first = message.find_first_not_of(" \t\n");
      if (first == std::string::npos) return std::nullopt;
      const size_t last = message.find_last_not_of(" \t\n");
      return message.substr(first, last - first + 1);
    }
    case ChatKey::kEscape:
      Clear();
      break;
    case ChatKey::kBackspace: {
      const size_t start = ctrl ? WordLeft() : (cursor_ > 0 ? cursor_ - 1 : 0);
      chars_.erase(chars_.begin() + start, chars_.begin() + cursor_);
      cursor_ = start;
      break;
    }
    case ChatKey::kDelete: {
      const size_t end = ctrl ? WordRight() : std::min(chars_.size(), cursor_ + 1);
      chars_.erase(chars_.begin() + cursor_, chars_.begin() + end);
      break;
    }
    case ChatKey::kLeft:
      cursor_ = ctrl ? WordLeft() : (cursor_ > 0 ? cursor_ - 1 : 0);
      break;
    case ChatKey::kRight:
      cursor_ = ctrl ? WordRight() : std::min(chars_.size(), cursor_ + 1);
      break;
    case ChatKey::kHome:
      cursor_ = 0;
      break;
    case ChatKey::kEnd:
      cursor_ = chars_.size();
      break;
    case ChatKey::kSelectAll:
      selected_all_ = !chars_.empty();
      break;
  }
  return std::nullopt;
}

}  // namespace nac
