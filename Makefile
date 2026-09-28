PKGS   = wayland-client wayland-egl wayland-cursor egl glesv2 xkbcommon freetype2 fontconfig
PREFIX ?= /usr/local

TOOLCHAIN ?= clang
ARCH      ?= $(shell uname -m)
CROSS     ?=
SYSROOT   ?=
CPU       ?= native
ARM_MARCH ?=
NATIVE    ?= 1
DEBUG     ?= 0
OPTLEVEL  ?= -O2

WARN = -Wall -Wextra -Wno-unused-parameter

ifeq ($(TOOLCHAIN),gcc)
CC     = $(CROSS)gcc
TC_OPT = -flto=auto -fuse-linker-plugin -fno-semantic-interposition
TC_LD  = -flto=auto -fuse-linker-plugin -Wl,-O2 -Wl,--gc-sections -Wl,--as-needed -s
else
CC     = $(if $(CROSS),clang --target=$(patsubst %-,%,$(CROSS)),clang)
TC_OPT = -flto=full -fno-semantic-interposition
TC_LD  = -flto=full -fuse-ld=lld -Wl,-O2 -Wl,--gc-sections -Wl,--as-needed \
         -Wl,--icf=safe -Wl,--build-id=none -s
endif

ifneq ($(CROSS),)
ifeq ($(CPU),native)
CPU := generic
endif
endif

ifneq ($(filter $(ARCH),aarch64 arm64),)
ifneq ($(ARM_MARCH),)
ARCHFLAGS = -march=$(ARM_MARCH) -mtune=$(CPU)
else
ARCHFLAGS = $(if $(filter 1,$(NATIVE)),$(if $(filter generic,$(CPU)),-march=armv8-a -moutline-atomics,-mcpu=$(CPU)),-march=armv8-a -moutline-atomics)
endif
else
ARCHFLAGS = $(if $(filter 1,$(NATIVE)),-march=$(CPU) -mtune=$(CPU),)
endif

OPT   = $(OPTLEVEL) $(ARCHFLAGS) $(TC_OPT) -fno-plt -fno-math-errno -fno-trapping-math \
        -fomit-frame-pointer -fvisibility=hidden -fmerge-all-constants \
        -ffunction-sections -fdata-sections -DNDEBUG
LDOPT = $(TC_LD)

ifeq ($(DEBUG),1)
OPT   = -O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined $(ARCHFLAGS)
LDOPT = -fsanitize=address,undefined
endif

PKGCONFIG = $(if $(SYSROOT),PKG_CONFIG_SYSROOT_DIR=$(SYSROOT) \
            PKG_CONFIG_LIBDIR=$(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/share/pkgconfig,) pkg-config
SYSFLAGS  = $(if $(SYSROOT),--sysroot=$(SYSROOT),)

PKG_OK = $(shell $(PKGCONFIG) --exists $(1) 2>/dev/null && echo y)

HAVE_PNG  := $(if $(or $(call PKG_OK,libpng),$(wildcard $(SYSROOT)/usr/include/png.h)),1,0)
HAVE_ZLIB := $(if $(or $(call PKG_OK,zlib),$(wildcard $(SYSROOT)/usr/include/zlib.h)),1,0)
PKGS += $(if $(filter 1,$(HAVE_PNG)),libpng) $(if $(filter 1,$(HAVE_ZLIB)),zlib)

# no .pc file: plain flags, the compiler complains if something is really missing
FALLBACK_CFLAGS_freetype2 = -I$(SYSROOT)/usr/include/freetype2
FALLBACK_LIBS_wayland-client  = -lwayland-client
FALLBACK_LIBS_wayland-egl     = -lwayland-egl
FALLBACK_LIBS_wayland-cursor  = -lwayland-cursor
FALLBACK_LIBS_egl             = -lEGL
FALLBACK_LIBS_glesv2          = -lGLESv2
FALLBACK_LIBS_xkbcommon       = -lxkbcommon
FALLBACK_LIBS_freetype2       = -lfreetype
FALLBACK_LIBS_fontconfig      = -lfontconfig
FALLBACK_LIBS_libpng          = -lpng
FALLBACK_LIBS_zlib            = -lz

PKG_FOUND   := $(foreach p,$(PKGS),$(if $(call PKG_OK,$(p)),$(p)))
PKG_MISSING := $(filter-out $(PKG_FOUND),$(PKGS))

ifneq ($(PKG_MISSING),)
ifeq ($(MAKELEVEL),0)
$(info pkg-config cannot find: $(PKG_MISSING) - building with plain -l flags)
endif
endif

CFLAGS  = -std=c11 -pipe $(WARN) $(OPT) $(SYSFLAGS) -D_GNU_SOURCE -I. -Iproto $(EXTRA_CFLAGS)
CFLAGS += $(if $(PKG_FOUND),$(shell $(PKGCONFIG) --cflags $(PKG_FOUND))) \
          $(foreach p,$(PKG_MISSING),$(FALLBACK_CFLAGS_$(p)))
LDFLAGS = $(LDOPT) $(SYSFLAGS) $(EXTRA_LDFLAGS)
LDLIBS  = $(if $(PKG_FOUND),$(shell $(PKGCONFIG) --libs $(PKG_FOUND))) \
          $(foreach p,$(PKG_MISSING),$(FALLBACK_LIBS_$(p))) -lutil -lm

WLP    ?= $(or $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null), \
               $(wildcard $(SYSROOT)/usr/share/wayland-protocols))

ifeq ($(strip $(WLP)),)
$(error cannot find wayland-protocols. Install it (Alpine: apk add wayland-protocols, \
Arch: pacman -S wayland-protocols) or set the path with WLP=/path/to/wayland-protocols)
endif

ifneq ($(CROSS),)
ifeq ($(strip $(SYSROOT)),)
$(error cross-compilation with CROSS=$(CROSS) also requires SYSROOT=/path/to/aarch64-root \
with wayland, EGL, freetype, fontconfig and xkbcommon for the target architecture)
endif
endif
KDEP    = /usr/share/plasma-wayland-protocols
SCANNER = wayland-scanner

EXTBG_XML = $(WLP)/staging/ext-background-effect/ext-background-effect-v1.xml
BLUR_XML  = $(KDEP)/blur.xml

HAVE_EXTBG := $(if $(wildcard $(EXTBG_XML)),1,0)
HAVE_KBLUR := $(if $(wildcard $(BLUR_XML)),1,0)

PRIMSEL_XML = $(WLP)/unstable/primary-selection/primary-selection-unstable-v1.xml
HAVE_PRIMSEL := $(if $(wildcard $(PRIMSEL_XML)),1,0)

TEXTIN_XML = $(WLP)/unstable/text-input/text-input-unstable-v3.xml
HAVE_TEXTIN := $(if $(wildcard $(TEXTIN_XML)),1,0)

PROTO_H = proto/xdg-shell-client-protocol.h proto/xdg-decoration-client-protocol.h
PROTO_C = proto/xdg-shell-protocol.c proto/xdg-decoration-protocol.c

ifeq ($(HAVE_PRIMSEL),1)
PROTO_H += proto/primary-selection-client-protocol.h
PROTO_C += proto/primary-selection-protocol.c
endif
ifeq ($(HAVE_TEXTIN),1)
PROTO_H += proto/text-input-client-protocol.h
PROTO_C += proto/text-input-protocol.c
endif

ifeq ($(HAVE_EXTBG),1)
PROTO_H += proto/ext-background-effect-client-protocol.h
PROTO_C += proto/ext-background-effect-protocol.c
endif
ifeq ($(HAVE_KBLUR),1)
PROTO_H += proto/blur-client-protocol.h
PROTO_C += proto/blur-protocol.c
endif

CFLAGS += -DHAVE_PNG=$(HAVE_PNG) -DHAVE_ZLIB=$(HAVE_ZLIB) -DHAVE_EXTBG=$(HAVE_EXTBG) -DHAVE_KBLUR=$(HAVE_KBLUR) -DHAVE_PRIMSEL=$(HAVE_PRIMSEL) -DHAVE_TEXTIN=$(HAVE_TEXTIN)

BIN ?= titty
VARIANTS = crt neon frost flat
CONFIGS  = $(addprefix titty.h.,$(VARIANTS))

SRC = main.c wayland.c pty.c vt.c font.c render.c boxdraw.c gfx.c $(PROTO_C)
OBJ = $(SRC:.c=.o)

all: $(BIN) logo

proto/xdg-shell-client-protocol.h: $(WLP)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p proto
	$(SCANNER) client-header $< $@

proto/xdg-shell-protocol.c: $(WLP)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p proto
	$(SCANNER) private-code $< $@

proto/xdg-decoration-client-protocol.h: $(WLP)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	@mkdir -p proto
	$(SCANNER) client-header $< $@

proto/xdg-decoration-protocol.c: $(WLP)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	@mkdir -p proto
	$(SCANNER) private-code $< $@

proto/ext-background-effect-client-protocol.h: $(EXTBG_XML)
	@mkdir -p proto
	$(SCANNER) client-header $< $@

proto/ext-background-effect-protocol.c: $(EXTBG_XML)
	@mkdir -p proto
	$(SCANNER) private-code $< $@

proto/text-input-client-protocol.h: $(TEXTIN_XML)
	@mkdir -p proto
	$(SCANNER) client-header $< $@

proto/text-input-protocol.c: $(TEXTIN_XML)
	@mkdir -p proto
	$(SCANNER) private-code $< $@

proto/primary-selection-client-protocol.h: $(PRIMSEL_XML)
	@mkdir -p proto
	$(SCANNER) client-header $< $@

proto/primary-selection-protocol.c: $(PRIMSEL_XML)
	@mkdir -p proto
	$(SCANNER) private-code $< $@

proto/blur-client-protocol.h: $(BLUR_XML)
	@mkdir -p proto
	$(SCANNER) client-header $< $@

proto/blur-protocol.c: $(BLUR_XML)
	@mkdir -p proto
	$(SCANNER) private-code $< $@

titty.h:
	@test -f titty.h.default || { echo "titty.h.default not found"; exit 1; }
	cp titty.h.default $@
	@echo "titty.h created from titty.h.default - edit it and run make again"

config_defaults.h: titty.h.default
	@awk '/^#define[ \t]+[A-Z0-9_]+/ { \
	    name = $$2; \
	    print "#ifndef " name; print $$0; print "#endif" ; next } \
	    { next }' titty.h.default > $@
	@echo "config_defaults.h generated from titty.h.default"

$(OBJ): $(PROTO_H) titty.h common.h config_defaults.h

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

define VARIANT_RULE
titty-$(1): presets/$(1).h titty.h common.h
	@+$$(MAKE) --no-print-directory clean-obj
	@+$$(MAKE) --no-print-directory BIN=titty-$(1) EXTRA_CFLAGS='-DTITTY_PRESET=\"presets/$(1).h\"'
	@+$$(MAKE) --no-print-directory clean-obj
endef

ifeq ($(BIN),titty)
$(foreach v,$(VARIANTS),$(eval $(call VARIANT_RULE,$(v))))
endif

FFLOGODIR = $(HOME)/.config/fastfetch/logo

logo: titty.logo
	@if [ -d "$(FFLOGODIR)" ]; then \
	  cp -f titty.logo "$(FFLOGODIR)/titty.logo"; \
	  rm -rf "$(HOME)/.cache/fastfetch/images"; \
	  echo "logo installed: $(FFLOGODIR)/titty.logo"; \
	fi

configs: $(CONFIGS)

$(CONFIGS): titty.h.%: presets/%.h titty.h preset_merge.py
	@python3 preset_merge.py $*

batshit: configs variants
	@echo
	@echo "== batshit done =="
	@printf '  binaries: '; for b in titty $(addprefix titty-,$(VARIANTS)); do \
	  test -f $$b && printf '%s ' $$b; done; echo
	@printf '  configs : '; for c in $(CONFIGS); do test -f $$c && printf '%s ' $$c; done; echo
	@echo "  switch theme: cp titty.h.crt titty.h && make"

variants: $(addprefix titty-,$(VARIANTS))
	@echo "done: $(addprefix titty-,$(VARIANTS))"

DEMO = flat frost crt

demo: $(BIN) $(addprefix titty-,$(DEMO))
	@echo "done: $(BIN) $(addprefix titty-,$(DEMO))"

PGODIR = $(CURDIR)/.pgo

pgo:
	@command -v llvm-profdata >/dev/null 2>&1 || { echo "llvm-profdata not found"; exit 1; }
	@[ -n "$$WAYLAND_DISPLAY" ] || { echo "pgo requires a wayland session"; exit 1; }
	$(MAKE) clean-obj
	rm -rf $(PGODIR) titty.profdata
	$(MAKE) EXTRA_CFLAGS="-fprofile-generate=$(PGODIR)" \
	        EXTRA_LDFLAGS="-fprofile-generate=$(PGODIR)"
	@echo "== profiling run =="
	@sh ./pgo-workload.sh ./titty >/dev/null 2>&1 || true
	@llvm-profdata merge -output=titty.profdata $(PGODIR)/*.profraw
	$(MAKE) clean-obj
	$(MAKE) EXTRA_CFLAGS="-fprofile-use=$(CURDIR)/titty.profdata \
	        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
	@rm -rf $(PGODIR)
	@echo "== PGO build done =="

gcc:
	+$(MAKE) clean-obj
	+$(MAKE) TOOLCHAIN=gcc

arm64:
	@if [ "$$(uname -m)" != "aarch64" ] && [ -z "$(CROSS)" ]; then \
	  @echo "make arm64 builds natively on aarch64 hardware."
	  @echo "You are on $$(uname -m) - cross-compilation required:"
	  @echo "  make arm64 CROSS=aarch64-linux-gnu- SYSROOT=/path/to/root CPU=cortex-a76"
	  @echo "SYSROOT must have aarch64 versions of wayland, EGL, freetype, fontconfig, xkbcommon."
	  exit 1; \
	fi
	+$(MAKE) clean-obj
	+$(MAKE) ARCH=aarch64

config:
	@echo "toolchain : $(TOOLCHAIN)  ($(CC))"
	@echo "arch      : $(ARCH)"
	@echo "archflags : $(ARCHFLAGS)"
	@echo "cpu       : $(CPU)"
	@echo "optlevel  : $(OPTLEVEL)"
	@echo "sysroot   : $(if $(SYSROOT),$(SYSROOT),-)"
	@echo "prefix    : $(PREFIX)"
	@echo "ext-bg-effect : $(if $(filter 1,$(HAVE_EXTBG)),yes,no)"
	@echo "kde-blur      : $(if $(filter 1,$(HAVE_KBLUR)),yes,no)"
	@echo "primary-sel   : $(if $(filter 1,$(HAVE_PRIMSEL)),yes,no)"
	@echo "text-input    : $(if $(filter 1,$(HAVE_TEXTIN)),yes,no)"

BINDIR  = $(DESTDIR)$(PREFIX)/bin
SHAREDIR = $(DESTDIR)$(PREFIX)/share
APPDIR  = $(SHAREDIR)/applications
DOCDIR  = $(SHAREDIR)/titty

install: titty
	install -Dm755 titty $(BINDIR)/titty
	@for v in $(VARIANTS); do \
	  if [ -f titty-$$v ]; then \
	    install -Dm755 titty-$$v $(BINDIR)/titty-$$v; \
	    echo "installed: $(BINDIR)/titty-$$v"; \
	  fi; \
	done
	install -Dm644 titty.desktop $(APPDIR)/titty.desktop
	@for v in $(VARIANTS); do \
	  if [ -f titty-$$v ]; then \
	    sed -e "s/^Name=.*/Name=titty ($$v)/" -e "s/^Exec=.*/Exec=titty-$$v/" \
	        -e "s/^TryExec=.*/TryExec=titty-$$v/" titty.desktop \
	      > $(APPDIR)/titty-$$v.desktop.tmp 2>/dev/null && \
	    install -Dm644 $(APPDIR)/titty-$$v.desktop.tmp $(APPDIR)/titty-$$v.desktop && \
	    rm -f $(APPDIR)/titty-$$v.desktop.tmp; \
	  fi; \
	done
	install -Dm644 titty.h $(DOCDIR)/titty.h
	@for v in $(VARIANTS); do \
	  install -Dm644 presets/$$v.h $(DOCDIR)/presets/$$v.h; \
	  test -f titty.h.$$v && install -Dm644 titty.h.$$v $(DOCDIR)/titty.h.$$v || true; \
	done
	@test -f titty.h.default && install -Dm644 titty.h.default $(DOCDIR)/titty.h.default || true
	@command -v update-desktop-database >/dev/null 2>&1 && \
	  update-desktop-database -q $(APPDIR) 2>/dev/null || true
	@echo "installed: $(BINDIR)/titty"

uninstall:
	rm -f $(BINDIR)/titty
	@for v in $(VARIANTS); do rm -f $(BINDIR)/titty-$$v; done
	rm -f $(APPDIR)/titty.desktop
	@for v in $(VARIANTS); do rm -f $(APPDIR)/titty-$$v.desktop; done
	rm -rf $(DOCDIR)
	@command -v update-desktop-database >/dev/null 2>&1 && \
	  update-desktop-database -q $(APPDIR) 2>/dev/null || true
	@echo "uninstalled"

clean-obj:
	rm -f $(OBJ)

clean: clean-obj
	rm -f config_defaults.h titty $(addprefix titty-,$(VARIANTS)) titty.profdata
	rm -rf proto $(PGODIR)

.PHONY: all clean clean-obj install uninstall pgo gcc arm64 config variants demo configs batshit logo
