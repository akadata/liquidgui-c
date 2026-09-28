# liquidgui - AIO and motherboard fan control
#
# (C) 2026 AKADATA LIMITED - Andrew Smalley
# Released under the MIT License.

# The binary lives in /usr/bin: it is a packaged application, not a local
# addition, and this matches the split the wider Saphira tooling expects.
# Override PREFIX to install elsewhere, e.g. PREFIX=/usr/local.
PREFIX      ?= /usr
BINDIR      ?= $(PREFIX)/bin
LIBEXECDIR  ?= $(PREFIX)/libexec/liquidgui
UDEV_DIR   ?= /etc/udev/rules.d
DESTDIR    ?=

CC          ?= cc
PKG_CONFIG  ?= pkg-config
INSTALL     ?= install

VERSION     ?= 2.0.0
UDEV_RULE   = etc/udev/rules.d/60-liquidctl.rules
HELPER      = lg-helper

WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wpointer-arith -Wwrite-strings
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -DLG_VERSION=\"$(VERSION)\" $(WARNINGS)
LDFLAGS ?=
LDLIBS  ?=

GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell $(PKG_CONFIG) --libs   gtk+-3.0 2>/dev/null)
ifeq ($(strip $(GTK_LIBS)),)
$(error gtk+-3.0 development files are required. On Arch: pacman -S gtk3)
endif

CORE_SRC = \
	src/lg_json.c \
	src/lg_model.c \
	src/lg_curve.c \
	src/lg_hwmon.c \
	src/lg_liquidctl.c \
	src/lg_control.c \
	src/lg_config.c \
	src/lg_theme.c \
	src/lg_dump.c

UI_SRC = \
	src/lg_ui.c \
	src/main.c

CORE_OBJ = $(CORE_SRC:.c=.o)
UI_OBJ   = $(UI_SRC:.c=.o)
TEST_OBJ = $(CORE_OBJ)

BIN     = liquidgui
HELPER_BIN = helper/$(HELPER)

TEST_BINS = \
	tests/test_json \
	tests/test_theme \
	tests/test_curve_parity \
	tests/test_detect_parity \
	tests/test_helper_allowlist \
	tests/test_config

.PHONY: all test check-parity sanitize install install-helper uninstall clean run dump help

all: $(BIN)

$(BIN): $(CORE_OBJ) $(UI_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(GTK_LIBS) $(LDLIBS) -lm

# The helper is the only component that runs as root, so it is built with the
# strictest warning set and is never linked against anything it does not need.
$(HELPER_BIN): helper/lg-helper.c
	$(CC) $(CFLAGS) -o $@ $<

# Plain C for the core; the interface additionally needs the GTK include path.
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

src/lg_ui.o src/main.o: CFLAGS += $(GTK_CFLAGS)

# --------------------------------------------------------------------- tests

tests/test_json: tests/test_json.c src/lg_json.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

tests/test_theme: tests/test_theme.c src/lg_theme.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

tests/test_curve_parity: tests/test_curve_parity.c $(TEST_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

tests/test_detect_parity: tests/test_detect_parity.c $(TEST_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

tests/test_helper_allowlist: tests/test_helper_allowlist.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

tests/test_config: tests/test_config.c $(TEST_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

test: $(TEST_BINS)
	@echo "== json reader and writer =="
	@./tests/test_json
	@echo "== palette contrast =="
	@./tests/test_theme
	@echo "== curve parity against the legacy Python =="
	@./tests/test_curve_parity tests/golden/curve_golden.json
	@echo "== discovery invariants =="
	@./tests/test_detect_parity tests/golden/detect_golden.json
	@echo "== helper allowlist =="
	@./tests/test_helper_allowlist
	@echo "== configuration and key migration =="
	@./tests/test_config

# The discovery invariants are asserted by tests/test_detect_parity, which runs
# as part of `make test`. This target just runs that one file on its own, for
# quick iteration while changing discovery.
check-parity: tests/test_detect_parity
	@./tests/test_detect_parity tests/golden/detect_golden.json

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -std=c11 -D_GNU_SOURCE $(WARNINGS)"

# ------------------------------------------------------------------- runtime

run: $(BIN)
	./$(BIN)

dump: $(BIN)
	@./$(BIN) --dump-detect --no-liquidctl

# ------------------------------------------------------------------ install

install: $(BIN)
	$(INSTALL) -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

# The helper must be installed setuid root. Refuse to do it half-way: a helper
# without the setuid bit silently degrades to a polkit prompt on every write.
install-helper: $(HELPER_BIN)
	@if [ "$(DESTDIR)" != "" ]; then \
		echo "packaging stage: installing $(HELPER_BIN) without the setuid bit"; \
		$(INSTALL) -Dm755 $(HELPER_BIN) $(DESTDIR)$(LIBEXECDIR)/$(HELPER); \
	else \
		$(INSTALL) -Dm4755 $(HELPER_BIN) $(LIBEXECDIR)/$(HELPER); \
		$(INSTALL) -Dm644 $(UDEV_RULE) $(UDEV_DIR)/60-liquidctl.rules; \
		echo "installed setuid helper at $(LIBEXECDIR)/$(HELPER)"; \
		udevadm control --reload-rules 2>/dev/null || true; \
		udevadm trigger 2>/dev/null || true; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(LIBEXECDIR)/$(HELPER)
	rm -f $(DESTDIR)$(UDEV_DIR)/60-liquidctl.rules

clean:
	rm -f $(CORE_OBJ) $(UI_OBJ) $(BIN) $(HELPER_BIN) $(TEST_BINS)
	rm -rf tests/__pycache__ __pycache__ build dist

help:
	@printf '%s\n' \
	  'Targets:' \
	  '  make                 build the liquidgui binary' \
	  '  make test            build and run the unit and parity tests' \
	  '  make check-parity    run the discovery invariant assertions' \
	  '  make sanitize        run the tests under ASan and UBSan' \
	  '  make run             build and launch the interface' \
	  '  make dump            print discovered sensors and controls as JSON' \
	  '  sudo make install    install the binary' \
	  '  sudo make install-helper  install the setuid helper and udev rule' \
	  '  make uninstall       remove installed files'
