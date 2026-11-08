CXX ?= c++
CXXFLAGS ?= -O2 -g
PREFIX ?= $(HOME)/.local

SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

LIB := build/libvox_menu.so
# Older names of the library, kept so existing setups still find it.
LEGACY_LIBS := build/libvox_tts.so build/libnightcap_ac.so

SRC := $(wildcard src/*.cc)
IMGUI := third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
	third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp \
	third_party/imgui/imgui_impl_vulkan.cpp
OBJ := $(SRC:src/%.cc=build/obj/%.o) $(IMGUI:third_party/imgui/%.cpp=build/obj/imgui/%.o)

# Everything is hidden except the SDL wrappers and the Vulkan layer entry
# point, so nothing else in Nightcap can bind to our copies.
FLAGS := -std=c++17 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden \
	-DVK_NO_PROTOTYPES -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS \
	-Ithird_party/imgui $(SDL_CFLAGS)

.PHONY: all build clean test install uninstall patch-desktop unpatch-desktop

all: build

build: $(LIB) $(LEGACY_LIBS) build/vox-menu-render

build/obj/%.o: src/%.cc $(wildcard src/*.h src/*.inc)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FLAGS) -Wall -Wextra -Wno-missing-field-initializers -c -o $@ $<

build/obj/imgui/%.o: third_party/imgui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FLAGS) -w -c -o $@ $<

$(LIB): $(OBJ) src/exports.map
	@mkdir -p build
	$(CXX) -shared -o $@ $(OBJ) $(SDL_LIBS) -ldl -lpthread -Wl,--no-undefined \
		-Wl,--version-script=src/exports.map

$(LEGACY_LIBS): $(LIB)
	ln -sf libvox_menu.so $@

build/vox-menu-render: tools/render.cc src/synth.cc src/synth.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -Wextra -o $@ tools/render.cc src/synth.cc -lm

TEST_BINS := build/vk_driver build/mic_driver build/chat_test build/automation_test build/xr_driver

build/vk_driver: tests/vk_driver.cc
	$(CXX) $(CXXFLAGS) -std=c++17 -o $@ $< $(SDL_CFLAGS) $(SDL_LIBS) -lvulkan

build/xr_driver: tests/xr_driver.cc
	$(CXX) $(CXXFLAGS) -std=c++17 -o $@ $< -lopenxr_loader -lvulkan

build/mic_driver: tests/mic_driver.cc
	$(CXX) $(CXXFLAGS) -std=c++17 -o $@ $< $(SDL_CFLAGS) $(SDL_LIBS)

build/chat_test: tests/chat_test.cc src/chat.cc src/chat.h
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -o $@ tests/chat_test.cc src/chat.cc

AUTOMATION_TEST_SRC := tests/automation_test.cc src/automation.cc src/config.cc src/app.cc src/overlay.cc \
	src/netstats.cc src/gamestate.cc src/nightcap.cc src/exe_symbols.cc src/servers.cc src/process.cc src/media.cc src/roblox_settings.cc \
	src/xr_math.cc

build/automation_test: $(AUTOMATION_TEST_SRC) $(wildcard src/*.h)
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -o $@ $(AUTOMATION_TEST_SRC) $(SDL_CFLAGS) $(SDL_LIBS) -ldl -lpthread

test: build $(TEST_BINS)
	./build/chat_test
	./build/automation_test
	python3 -m unittest discover -s tests -v

# --- install -------------------------------------------------------------------

BINDIR := $(PREFIX)/bin
DATA_HOME := $(or $(XDG_DATA_HOME),$(HOME)/.local/share)
DESKTOP_ID := io.github.CoderDayton.nightcap.desktop
DESKTOP_TARGET := $(DATA_HOME)/applications/$(DESKTOP_ID)
# Nightcap's own launcher entry (native first, then Flatpak).
DESKTOP_SOURCE ?= $(firstword $(wildcard \
	/usr/local/share/applications/$(DESKTOP_ID) \
	/usr/share/applications/$(DESKTOP_ID) \
	$(DATA_HOME)/flatpak/exports/share/applications/$(DESKTOP_ID) \
	/var/lib/flatpak/exports/share/applications/$(DESKTOP_ID)))
MARKER := X-Vox-Menu=true
# Markers written by older versions; their patched entries are ours too.
ANY_MARKER := ^X-(Vox-Menu|Vox-TTS|Nightcap-AC)=true$$

install: build
	install -Dm755 $(LIB) $(PREFIX)/lib/vox-menu/libvox_menu.so
	strip --strip-debug $(PREFIX)/lib/vox-menu/libvox_menu.so
	install -Dm755 scripts/vox-menu $(BINDIR)/vox-menu
	install -Dm755 scripts/vox-menu-download-voice $(BINDIR)/vox-menu-download-voice
	@# Older command names, so launchers patched by earlier versions keep working.
	ln -sf vox-menu $(BINDIR)/vox-tts
	ln -sf vox-menu $(BINDIR)/nightcap-ac
	ln -sf vox-menu-download-voice $(BINDIR)/vox-tts-download-voice
	install -Dm644 packaging/vox_menu_layer.json.in $(PREFIX)/share/vox-menu/vox_menu_layer.json.in
	install -Dm644 packaging/vox_menu_xr_layer.json.in $(PREFIX)/share/vox-menu/vox_menu_xr_layer.json.in
	install -Dm644 packaging/vox-menu.desktop $(PREFIX)/share/applications/vox-menu.desktop
	install -Dm644 assets/vox-menu.svg $(PREFIX)/share/icons/hicolor/scalable/apps/vox-menu.svg
	@mkdir -p $(PREFIX)/share/vox-menu/voices/animalese/english
	@mkdir -p $(PREFIX)/share/vox-menu/voices/animalese/korean
	install -Dm644 assets/voices/animalese/english/*.ogg $(PREFIX)/share/vox-menu/voices/animalese/english/
	install -Dm644 assets/voices/animalese/korean/*.ogg $(PREFIX)/share/vox-menu/voices/animalese/korean/
	@# Clean up what older versions installed
	rm -rf $(PREFIX)/lib/vox-tts $(PREFIX)/lib/nightcap-ac $(PREFIX)/share/vox-tts $(PREFIX)/share/nightcap-ac
	rm -f $(PREFIX)/share/applications/vox-tts.desktop $(PREFIX)/share/applications/nightcap-ac.desktop
	rm -f $(PREFIX)/share/applications/nightcap-ac-roblox.desktop $(PREFIX)/share/icons/hicolor/scalable/apps/vox-tts.svg
	@if grep -qxE '$(ANY_MARKER)' '$(DESKTOP_TARGET)' 2>/dev/null; then \
	  $(MAKE) --no-print-directory patch-desktop; \
	else \
	  answer='$(or $(VOX_MENU_DESKTOP),$(VOX_TTS_DESKTOP),$(NIGHTCAP_AC_DESKTOP))'; \
	  if [ -z "$$answer" ] && [ -t 0 ]; then \
	    echo; \
	    echo "Patch Nightcap's launcher so the Nightcap app entry and roblox:// links"; \
	    echo "start Nightcap with Vox Menu?"; \
	    echo "This writes a copy of Nightcap's entry to $(DESKTOP_TARGET)."; \
	    echo "Undo any time with 'make unpatch-desktop'."; \
	    printf 'Patch it? [y/N] '; read -r answer; \
	  fi; \
	  case "$$answer" in \
	    y|Y|yes) $(MAKE) --no-print-directory patch-desktop ;; \
	    *) echo "Skipped the launcher patch. Run 'make patch-desktop' to add it later." ;; \
	  esac; \
	fi

patch-desktop:
	@[ -n '$(DESKTOP_SOURCE)' ] || { echo "Nightcap's desktop entry was not found; is Nightcap installed?"; exit 1; }
	@mkdir -p '$(dir $(DESKTOP_TARGET))'
	@if [ -e '$(DESKTOP_TARGET)' ] && ! grep -qxE '$(ANY_MARKER)' '$(DESKTOP_TARGET)'; then \
	  cp -p '$(DESKTOP_TARGET)' '$(DESKTOP_TARGET).vox-menu-backup'; \
	fi
	@awk -v launcher='$(BINDIR)/vox-menu' -v marker='$(MARKER)' ' \
	  /^\[/ { group = $$0; print; if (group == "[Desktop Entry]") print marker; next } \
	  group == "[Desktop Entry]" && ($$0 == "X-Mocktail-Managed=true" || $$0 ~ /^X-(Vox-Menu|Vox-TTS|Nightcap-AC)=true$$/) { next } \
	  group == "[Desktop Entry]" && /^Exec=/ && !done { print "Exec=env SDL_VIDEODRIVER=wayland,x11 " launcher " %u"; done = 1; next } \
	  { print }' '$(DESKTOP_SOURCE)' > '$(DESKTOP_TARGET).tmp'
	@chmod 644 '$(DESKTOP_TARGET).tmp' && mv '$(DESKTOP_TARGET).tmp' '$(DESKTOP_TARGET)'
	@-update-desktop-database -q '$(dir $(DESKTOP_TARGET))' 2>/dev/null || true
	@-xdg-mime default $(DESKTOP_ID) x-scheme-handler/roblox x-scheme-handler/roblox-player 2>/dev/null || true
	@echo "Patched $(DESKTOP_TARGET) (from $(DESKTOP_SOURCE))"

unpatch-desktop:
	@if [ ! -e '$(DESKTOP_TARGET)' ]; then echo "No launcher patch installed"; \
	elif ! grep -qxE '$(ANY_MARKER)' '$(DESKTOP_TARGET)'; then echo "Left $(DESKTOP_TARGET) alone: not written by Vox Menu"; \
	else \
	  rm -f '$(DESKTOP_TARGET)'; \
	  restored=; \
	  for backup in vox-menu-backup vox-tts-backup nightcap-ac-backup; do \
	    if [ -z "$$restored" ] && [ -e '$(DESKTOP_TARGET)'."$$backup" ]; then \
	      mv '$(DESKTOP_TARGET)'."$$backup" '$(DESKTOP_TARGET)'; restored=1; \
	      echo "Restored your previous $(DESKTOP_TARGET)"; \
	    fi; \
	  done; \
	  [ -n "$$restored" ] || echo "Removed $(DESKTOP_TARGET); the system Nightcap entry is used again"; \
	  update-desktop-database -q '$(dir $(DESKTOP_TARGET))' 2>/dev/null || true; \
	fi

uninstall: unpatch-desktop
	rm -f $(BINDIR)/vox-menu $(BINDIR)/vox-menu-download-voice
	rm -f $(BINDIR)/vox-tts $(BINDIR)/nightcap-ac $(BINDIR)/vox-tts-download-voice
	rm -f $(PREFIX)/share/applications/vox-menu.desktop $(PREFIX)/share/icons/hicolor/scalable/apps/vox-menu.svg
	rm -rf $(PREFIX)/lib/vox-menu $(PREFIX)/share/vox-menu

clean:
	rm -rf build
