// Unit tests for the chat-box tracker.

#include <cstdio>
#include <string>

#include "../src/chat.h"

using nac::ChatKey;
using nac::ChatTracker;

static int failures = 0;

static void Expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  {
    ChatTracker t;
    t.Open(false, false);
    t.Text("hello");
    t.Text(" world");
    auto sent = t.Key(ChatKey::kReturn, false);
    Expect(sent && *sent == "hello world", "typed message is sent on Enter");
    Expect(t.last_length() == 11, "last_length records character count");
  }
  {
    // Editing keys, including word jumps and deletes.
    ChatTracker t;
    t.Open(false, false);
    t.Text("helo wrld");
    t.Key(ChatKey::kLeft, true);
    t.Key(ChatKey::kRight, false);
    t.Text("o");
    t.Key(ChatKey::kHome, false);
    for (int i = 0; i < 3; ++i) t.Key(ChatKey::kRight, false);
    t.Text("l");
    t.Key(ChatKey::kEnd, false);
    t.Text(" foo");
    t.Key(ChatKey::kBackspace, true);
    t.Key(ChatKey::kBackspace, false);
    auto sent = t.Key(ChatKey::kReturn, false);
    Expect(sent && *sent == "hello world", "editing keys rebuild the text");
  }
  {
    ChatTracker t;
    t.Open(false, false);
    t.Text("nope");
    t.Key(ChatKey::kEscape, false);
    Expect(!t.Key(ChatKey::kReturn, false), "Escape discards the draft");
    t.Text("also no");
    t.Close();
    t.Text("closed");
    Expect(!t.Key(ChatKey::kReturn, false), "closing the box discards the draft");
  }
  {
    ChatTracker t;
    t.Open(true, false);
    t.Text("hunter2");
    Expect(!t.Key(ChatKey::kReturn, false), "password boxes are never tracked");
  }
  {
    ChatTracker t;
    t.Open(false, false);
    t.Paste("copied\ntext");
    auto sent = t.Key(ChatKey::kReturn, false);
    Expect(sent && *sent == "copied text", "pasted line breaks become spaces");
    t.Open(false, true);
    t.Text("a");
    Expect(!t.Key(ChatKey::kReturn, false), "Enter in a multi-line box is a newline");
  }
  {
    ChatTracker t;
    t.Open(false, false);
    t.Text("héllo 😀");
    t.Key(ChatKey::kBackspace, false);
    auto sent = t.Key(ChatKey::kReturn, false);
    Expect(sent && *sent == "héllo", "backspace removes whole UTF-8 characters");
  }
  {
    // Ctrl+A then typing replaces everything (the VR keyboard's Select All).
    ChatTracker t;
    t.Open(false, false);
    t.Text("old text");
    t.Key(ChatKey::kSelectAll, false);
    t.Text("new");
    auto sent = t.Key(ChatKey::kReturn, false);
    Expect(sent && *sent == "new", "typing after select-all replaces the text");
  }
  {
    ChatTracker t;
    t.Open(false, false);
    t.Text("gone");
    t.Key(ChatKey::kSelectAll, false);
    t.Key(ChatKey::kBackspace, false);
    t.Text("hi");
    auto sent = t.Key(ChatKey::kReturn, false);
    Expect(sent && *sent == "hi", "backspace after select-all empties the box");
  }
  {
    ChatTracker t;
    t.Open(false, false);
    t.Text("bc");
    t.Key(ChatKey::kSelectAll, false);
    t.Key(ChatKey::kLeft, false);
    t.Text("a");
    auto sent = t.Key(ChatKey::kReturn, false);
    Expect(sent && *sent == "abc", "left after select-all drops it at the start");
  }
  std::printf(failures ? "chat_test: %d failure(s)\n" : "chat_test: all passed\n", failures);
  return failures ? 1 : 0;
}
