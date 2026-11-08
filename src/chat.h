// Rebuilds what is in the focused Roblox text box from its keystrokes.
//
// Roblox closes the chat bar after Enter, so a message is the box's contents
// at the moment Enter is pressed. Escape or losing focus throws the draft
// away, as in-game. Password boxes are never tracked.

#ifndef VOX_MENU_CHAT_H_
#define VOX_MENU_CHAT_H_

#include <optional>
#include <string>
#include <vector>

namespace nac {

enum class ChatKey {
  kReturn, kEscape, kBackspace, kDelete, kLeft, kRight, kHome, kEnd,
  kSelectAll,  // Ctrl+A
};

class ChatTracker {
 public:
  void Open(bool secret, bool multiline);
  void Close();
  void Text(const std::string& utf8);
  void Paste(const std::string& utf8);
  // Returns the message when the key sends it.
  std::optional<std::string> Key(ChatKey key, bool ctrl);

  bool active() const { return active_; }
  size_t last_length() const { return last_length_; }

 private:
  void Clear();
  void Insert(const std::vector<std::string>& chars);
  size_t WordLeft() const;
  size_t WordRight() const;

  bool active_ = false;
  bool secret_ = false;
  bool multiline_ = false;
  size_t last_length_ = 0;
  std::vector<std::string> chars_;  // one UTF-8 character per entry
  size_t cursor_ = 0;
  bool selected_all_ = false;  // after Ctrl+A, until the next edit or move
};

}  // namespace nac

#endif  // VOX_MENU_CHAT_H_
