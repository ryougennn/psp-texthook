# PSP texthook: builds build/thkcore.prx (kernel: capture and the rest),
# build/texthook.prx (user: the network) and build/thkpopup.prx (user: the
# dictionary popup).
#
#   make                 build all three
#   make install MS=...  copy them to a mounted memory stick
#   make LOG_FILE=0      log to stdout (PSPLink) only, not to the memory stick
#   make dist            build/texthook.zip, laid out like the memory stick root
#   make dict JMDICT=JMdict_e.gz KANJIDIC=kanjidic2.xml.gz
#                        build/dicts/: the popup's dictionaries, JMdict and VNDB
#                        Names (from the third_party/vndb-names submodule)
#   make clean

PSPSDK := $(shell psp-config --pspsdk-path)

CC    := psp-gcc
FIXUP := psp-fixup-imports
PRXGEN := psp-prxgen
EXPGEN := psp-build-exports

BUILD := build

LOG_FILE ?= 1

CFLAGS := -Os -G0 -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -fno-pic -ffreestanding -fno-tree-loop-distribute-patterns \
          -D_PSP_FW_VERSION=660 \
          -Iinclude -Icommon -Ishared -Ithird_party/qrcodegen -Ithird_party/stb -I$(PSPSDK)/include -MMD -MP
ifeq ($(LOG_FILE),1)
CFLAGS += -DTH_LOG_FILE
endif

LDFLAGS := -L$(PSPSDK)/lib -specs=$(PSPSDK)/lib/prxspecs \
           -Wl,-q,-T$(PSPSDK)/lib/linkfile.prx -Wl,-zmax-page-size=128 \
           -nostartfiles -nostdlib

# Every games/*.c is a hook script; the Makefile generates a registry of
# them, so adding a game is just adding a file. Each is compiled twice: into
# the core, which runs its hooks, and with TH_FILTERS_ONLY into the user
# modules, which run its filters.
GAME_SRCS  := $(wildcard games/*.c)
GAME_NAMES := $(basename $(notdir $(GAME_SRCS)))

# Kernel core. The *_660 stub libraries carry the 6.60 kernel NIDs that
# ARK-5 expects; they must come before -lpspkernel. filters.o is only there
# because the scripts' filters are compiled in with their hooks (the user
# modules run them).
CORE_OBJS := $(BUILD)/core/main.o $(BUILD)/core/hook.o $(BUILD)/core/capture.o \
             $(BUILD)/core/hook_entry.o $(BUILD)/core/overlay.o $(BUILD)/core/popup.o \
             $(BUILD)/core/exports.o $(BUILD)/core/games_registry.o \
             $(BUILD)/shared/filters.o $(BUILD)/common/mini.o \
             $(BUILD)/third_party/qrcodegen/qrcodegen.o \
             $(patsubst %,$(BUILD)/core/games/%.o,$(GAME_NAMES))
CORE_LIBS := -lpspsystemctrl_kernel -lpsploadcore_kernel_660 \
             -lpspmodulemgr_kernel_660 -lpspsysmem_kernel_660 -lpspthreadman_kernel_660 \
             -lpspinterruptmanager_kernel_660 -lpspmodinfo -lpspsdk -lpspkernel \
             -lpspwlan -lpsppower -lpspuser -lgcc

# What both user modules share: decoding, the scripts' filters, logging.
SHARED_OBJS := $(BUILD)/shared/text.o $(BUILD)/shared/filters.o $(BUILD)/shared/script.o \
               $(BUILD)/shared/log.o $(BUILD)/shared/games_registry.o $(BUILD)/common/mini.o \
               $(BUILD)/thkcore_stubs.o \
               $(patsubst %,$(BUILD)/shared/games/%.o,$(GAME_NAMES))

# The network module.
USER_OBJS := $(BUILD)/user/main.o $(BUILD)/user/exports.o $(BUILD)/user/lines.o \
             $(BUILD)/user/config.o $(BUILD)/user/net.o $(BUILD)/user/http.o \
             $(BUILD)/user/shot.o $(BUILD)/user/mdns.o \
             $(BUILD)/user/session.o $(BUILD)/third_party/stb/stb_jpeg.o \
             $(SHARED_OBJS)
USER_LIBS := -lpspmodinfo -lpspnet_apctl -lpspnet_inet -lpspnet \
             -lpsputility -lpspreg -lpspdisplay -lpspdmac -lpspwlan -lpspuser -lgcc

# The dictionary popup.
POPUP_OBJS := $(BUILD)/popup/popup.o $(BUILD)/popup/popup_draw.o $(BUILD)/popup/dict.o \
              $(BUILD)/popup/pgf.o $(BUILD)/popup/exports.o \
              $(SHARED_OBJS)
POPUP_LIBS := -lpspmodinfo -lpspctrl -lpspdisplay -lpspdmac -lpsputility -lpspuser -lgcc

all: $(BUILD)/thkcore.prx $(BUILD)/texthook.prx $(BUILD)/thkpopup.prx $(BUILD)/sjis.tbl

# --- linking -----------------------------------------------------------------

$(BUILD)/%.prx: $(BUILD)/%.elf
	$(PRXGEN) $< $@

$(BUILD)/thkcore.elf: $(CORE_OBJS)
	$(CC) $(LDFLAGS) $^ $(CORE_LIBS) -o $@
	$(FIXUP) $@

$(BUILD)/texthook.elf: $(USER_OBJS)
	$(CC) $(LDFLAGS) $^ $(USER_LIBS) -o $@
	$(FIXUP) $@

$(BUILD)/thkpopup.elf: $(POPUP_OBJS)
	$(CC) $(LDFLAGS) $^ $(POPUP_LIBS) -o $@
	$(FIXUP) $@

# --- compiling ---------------------------------------------------------------

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Vendored code: compiled as-is, asserts off, its own warnings tolerated.
$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DNDEBUG -w -c $< -o $@

$(BUILD)/core/games/%.o: games/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DTH_GAME_SYM=th_game_$* -c $< -o $@

$(BUILD)/shared/games/%.o: games/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DTH_GAME_SYM=th_game_$* -DTH_FILTERS_ONLY -c $< -o $@

$(BUILD)/%.o: $(BUILD)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Export tables for each module, and the stubs the user module imports
# from the core.
$(BUILD)/%/exports.c: %/exports.exp
	@mkdir -p $(dir $@)
	$(EXPGEN) -b $< > $@

$(BUILD)/thkcore_stubs.S: core/exports.exp
	@mkdir -p $(BUILD)/stubs
	cd $(BUILD)/stubs && $(EXPGEN) -s $(CURDIR)/$<
	mv $(BUILD)/stubs/thkcore.S $@

$(BUILD)/thkcore_stubs.o: $(BUILD)/thkcore_stubs.S
	$(CC) $(CFLAGS) -c $< -o $@

# Rewritten on every run, but only replaced when the list of games changed,
# so adding or removing a games/*.c file relinks without a full rebuild.
$(BUILD)/core/games_registry.c $(BUILD)/shared/games_registry.c: FORCE
	@mkdir -p $(dir $@)
	@{ echo '/* Generated by the Makefile from the games directory. Do not edit. */'; \
	   echo '#include "texthook.h"'; \
	   for g in $(GAME_NAMES); do echo "extern const th_game th_game_$$g;"; done; \
	   echo 'const th_game *const th_games[] = {'; \
	   for g in $(GAME_NAMES); do echo "    &th_game_$$g,"; done; \
	   echo '    0'; \
	   echo '};'; } > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

# Shift-JIS -> Unicode table, loaded from the memory stick on demand.
$(BUILD)/sjis.tbl: tools/gen_sjis_table.py
	@mkdir -p $(dir $@)
	python3 $< $@

# The popup's dictionaries: JMdict from EDRDG's files
# (http://ftp.edrdg.org/pub/Nihongo/, downloaded by hand, so not part of `all`),
# and VNDB Names from the third_party/vndb-names submodule. Other Yomitan
# dictionaries converted with tools/yomitan2dict.py into build/dicts/ are
# installed too. Their licences (tools/DICT-LICENSES.txt) go wherever they're kept.
VNDB_ZIP := third_party/vndb-names/VNDB Names.zip

dict: $(BUILD)/dicts/1-jmdict.bin $(BUILD)/dicts/2-vndb-names.bin $(BUILD)/dicts/LICENSES.txt

$(BUILD)/dicts/LICENSES.txt: tools/DICT-LICENSES.txt
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/dicts/1-jmdict.bin: tools/gen_dict.py
	@test -n "$(JMDICT)" -a -n "$(KANJIDIC)" || { \
	    echo "usage: make dict JMDICT=path/JMdict_e.gz KANJIDIC=path/kanjidic2.xml.gz"; exit 1; }
	@mkdir -p $(dir $@)
	python3 $< $(JMDICT) $(KANJIDIC) $@

$(BUILD)/dicts/2-vndb-names.bin: tools/yomitan2dict.py tools/gen_dict.py
	@test -f "$(VNDB_ZIP)" || { \
	    echo "third_party/vndb-names is empty: git submodule update --init"; exit 1; }
	@mkdir -p $(dir $@)
	python3 $< "$(VNDB_ZIP)" $@

# --- installing --------------------------------------------------------------

# MS is the memory stick's mount point, e.g. make install MS=/run/media/$USER/PSP
install: all
	@test -n "$(MS)" || { echo "usage: make install MS=/path/to/memstick"; exit 1; }
	mkdir -p "$(MS)/SEPLUGINS/texthook"
	cp $(BUILD)/thkcore.prx $(BUILD)/texthook.prx $(BUILD)/thkpopup.prx $(BUILD)/sjis.tbl \
	   "$(MS)/SEPLUGINS/texthook/"
	@# The web page is served from the stick; don't overwrite one already there.
	mkdir -p "$(MS)/SEPLUGINS/texthook/web"
	test -e "$(MS)/SEPLUGINS/texthook/web/index.html" || cp web/index.html "$(MS)/SEPLUGINS/texthook/web/"
	cp web/favicon.svg "$(MS)/SEPLUGINS/texthook/web/"
	@# Dictionaries are big (JMdict is 28 MB): only copied when changed.
	@for d in $(wildcard $(BUILD)/dicts/*.bin); do \
	    mkdir -p "$(MS)/SEPLUGINS/texthook/dicts"; \
	    t="$(MS)/SEPLUGINS/texthook/dicts/$$(basename $$d)"; \
	    cmp -s "$$d" "$$t" || { echo "cp $$d $$t"; cp "$$d" "$$t"; }; \
	done
	@# With them, their licences.
	@test ! -d "$(MS)/SEPLUGINS/texthook/dicts" || \
	    cp tools/DICT-LICENSES.txt "$(MS)/SEPLUGINS/texthook/dicts/LICENSES.txt"
	@echo
	@echo "Make sure $(MS)/SEPLUGINS/PLUGINS.TXT has this line (and no texthook.prx or"
	@echo "thkpopup.prx line: the core loads them itself when they're needed):"
	@echo "  game, ms0:/SEPLUGINS/texthook/thkcore.prx, on"

# A zip laid out like the memory stick root, for releases. PLUGINS.TXT is
# left out on purpose: the user already has one with other plugins in it.
DIST := $(BUILD)/dist
dist: all
	rm -rf $(DIST) $(BUILD)/texthook.zip
	mkdir -p $(DIST)/SEPLUGINS/texthook/web
	cp $(BUILD)/thkcore.prx $(BUILD)/texthook.prx $(BUILD)/thkpopup.prx $(BUILD)/sjis.tbl \
	   config.ini.example $(DIST)/SEPLUGINS/texthook/
	cp web/index.html web/favicon.svg $(DIST)/SEPLUGINS/texthook/web/
	@# The dictionaries, when built (make dict; CI builds JMdict), with their licences.
	@if ls $(BUILD)/dicts/*.bin >/dev/null 2>&1; then \
	    mkdir -p $(DIST)/SEPLUGINS/texthook/dicts; \
	    echo "cp $(BUILD)/dicts/*.bin $(DIST)/SEPLUGINS/texthook/dicts/"; \
	    cp $(BUILD)/dicts/*.bin $(DIST)/SEPLUGINS/texthook/dicts/; \
	    cp tools/DICT-LICENSES.txt $(DIST)/SEPLUGINS/texthook/dicts/LICENSES.txt; \
	fi
	cd $(DIST) && zip -r -X ../texthook.zip SEPLUGINS

clean:
	rm -rf $(BUILD)

FORCE:
.PHONY: all dict install dist clean FORCE
.SECONDARY:

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
