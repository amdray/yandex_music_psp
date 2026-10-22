TARGET = ympsp
# BUILD_PRX=1 — сборка PRX для отладки через PSPLINK (гайд pspdev: debugging).
# DEBUG=1 — без оптимизации и с -g3 (иначе psp-addr2line/psp-gdb не видят строк).
# Пример: make clean && make BUILD_PRX=1 DEBUG=1   -> ympsp.prx + ympsp.elf (с символами)
BUILD_PRX ?= 0
DEBUG ?= 0
OBJS = src/main.o \
       src/app/app_state.o \
       src/ui/ui_screens.o \
       src/ui/ui_draw.o \
       src/ui/ui_layout.o \
       src/ui/ui_icon_atlas.o \
       src/ui/ui_common.o \
       src/ui/ui_screen_splash.o \
       src/ui/ui_screen_menu.o \
       src/ui/ui_screen_playlist_list.o \
       src/ui/ui_screen_account.o \
       src/ui/ui_screen_now_playing.o \
       src/ui/ui_screen_album_list.o \
       src/ui/ui_screen_track_list.o \
       src/ui/ui_screen_artist.o \
       src/ui/ui_screen_artist_menu.o \
       src/ui/ui_screen_net_info.o \
       src/ui/ui_screen_device_login.o \
       src/ui/ui_screen_wave.o \
       src/ui/ui_screen_search.o \
       src/ui/ui_wave_carousel.o \
       src/ui/ui_glow_bg.o \
       src/ui/ui_screen_help.o \
       src/ui/ui_screen_memory.o \
       src/ui/ui_screen_eq.o \
       src/hal/hal_input.o \
       src/hal/hal_gpu.o \
       src/core/fs.o \
       src/core/logger.o \
       src/core/mem_probe.o \
       src/fonts/cbmf.o \
       src/fonts/cbmf_psp.o \
       src/fonts/cbmf_fonts.o \
       src/fonts/text.o \
       src/services/image_loader.o \
       src/services/locale.o \
       src/services/token_loader.o \
       src/services/ya_auth.o \
       src/services/last_play.o \
       src/services/track_like.o \
       src/services/ym_api_like.o \
       src/services/ym_api_genres.o \
       src/services/ym_api_albums.o \
       src/services/ym_api_wave.o \
       src/services/ym_api_rotor.o \
       src/services/ym_api_track_parse.o \
       src/services/ym_api_search.o \
       src/services/osk_input.o \
       src/services/playback_reporter.o \
       src/services/wave.o \
       src/services/album_play.o \
       src/services/eq.o \
       src/services/qrcodegen.o \
       src/services/splash_flow.o \
       src/services/net_stack.o \
       src/services/net_activity.o \
       src/services/net_ui_status.o \
       src/services/time_sync.o \
       src/services/dns_wire.o \
       src/services/dns.o \
       src/services/net_tls.o \
       src/services/net_http.o \
       src/services/net_client.o \
       src/services/library.o \
       src/services/systemctrl_rng.o \
       src/services/systemctrl_rng_stub.o \
       src/services/system_status.o \
       src/services/kubridge_call_stub.o \
       src/services/ym_api_download.o \
       src/services/ym_flac.o \
       src/services/ym_aac.o \
       src/services/mp4_track.o \
       src/services/ym_api_playlists.o \
       src/services/api_parser.o \
       src/services/list_index.o \
       src/services/track_meta_store.o \
       src/services/track_hydrator.o \
       src/services/cover_storage.o \
       src/services/cover_cache.o \
       src/services/resource_policy.o \
       src/services/cover_http_client.o \
       src/services/cover_manager.o \
       src/services/cover_now_playing.o \
       src/services/video_cover.o \
       src/services/track_download.o \
       src/services/audio_stream_buf.o \
       src/services/audio_cache.o \
       src/services/audio_player.o \
       src/services/playback_queue.o \
       src/services/playback_controller.o \
       src/services/speed_test.o \
       src/services/video_player.o
EXIT_STAGE ?= 0
PSP_CONFIG ?= psp-config
PSPSDK ?= $(shell $(PSP_CONFIG) --pspsdk-path)
RELEASE_DIR = release
FONT_RESOURCES = $(wildcard fonts/*)
ASSET_RESOURCES = $(filter-out assets/icon/%.gif assets/icon/%.svg assets/icon/%_preview.png assets/icon/in_explicit.png,$(shell find assets -type f))
PACKAGED_FONT_RESOURCES = $(patsubst fonts/%,$(RELEASE_DIR)/fonts/%,$(FONT_RESOURCES))
PACKAGED_ASSET_RESOURCES = $(patsubst assets/%,$(RELEASE_DIR)/assets/%,$(ASSET_RESOURCES))
RELEASE_TOKEN = $(RELEASE_DIR)/config/token.txt
PBP_ICON = branding/ICON0.PNG

INCDIR = include vendor/faad2-master/include

ifeq ($(DEBUG),1)
OPT_FLAGS = -O0 -g3
else
OPT_FLAGS = -O2
endif
CFLAGS = $(OPT_FLAGS) -G0 -Wall -Wextra -Wshadow -std=c99 -MMD -MP -DEXIT_STAGE=$(EXIT_STAGE)
ifeq ($(BUILD_PRX),1)
# PRX-сборка: PBP/release-ресурсы не нужны (запуск с host0:/ через PSPLINK).
CFLAGS += -DYM_PSPLINK_DEBUG=1
EXTRA_TARGETS =
else
EXTRA_TARGETS = $(RELEASE_DIR)/EBOOT.PBP $(PACKAGED_FONT_RESOURCES) $(PACKAGED_ASSET_RESOURCES)
endif
PSP_EBOOT_TITLE = YMPSP

# pspdebug/pspdisplay/pspge/pspctrl/pspnet/pspnet_apctl НЕ указывать здесь:
# build.mak добавляет их последними сам; дубль раньше по строке рвёт группы
# импорт-стабов (psp-fixup-imports: "stubs out of order").
LIBS = $(FAAD_LIB) -lmbedtls -lmbedx509 -lmbedcrypto \
    -lpspgu -lpspnet_inet -lpspwlan -lpsputility -lpsppower -lpspaudio -lpspmp3 \
    -lpspmpeg \
    -ljpeg -lpng -lz -lcjson -lm

# libcglue (неявный -lc в конце линковки) ссылается на sceUtilityGetSystemParamInt
# из timezone.o; -u втягивает этот стаб в раннюю группу -lpsputility, иначе
# поздняя ссылка открывает вторую группу того же модуля (stubs out of order).
LDFLAGS += -Wl,-u,sceUtilityGetSystemParamInt

# --- FAAD2 (AAC, GPLv2; см. vendor/faad2-master/COPYING) ---
# Явные правила как в native: паттерн %.o: %.c из build.mak не применяется.
FAAD_DIR = vendor/faad2-master
FAAD_OBJS = $(patsubst %.c,%.o,$(wildcard $(FAAD_DIR)/libfaad/*.c))
FAAD_LIB = $(FAAD_DIR)/libfaad/libfaad.a
FAAD_CFLAGS = -O2 -G0 '-DPACKAGE_VERSION="2.11.2-ym"' -I$(FAAD_DIR)/include -I$(FAAD_DIR)/libfaad

# Явные правила faad стоят ДО include build.mak — без этой строки первая
# цель файла (bits.o) стала бы goal по умолчанию и make остановился на ней.
.DEFAULT_GOAL := all

$(FAAD_DIR)/libfaad/bits.o: $(FAAD_DIR)/libfaad/bits.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/cfft.o: $(FAAD_DIR)/libfaad/cfft.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/common.o: $(FAAD_DIR)/libfaad/common.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/decoder.o: $(FAAD_DIR)/libfaad/decoder.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/drc.o: $(FAAD_DIR)/libfaad/drc.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/drm_dec.o: $(FAAD_DIR)/libfaad/drm_dec.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/error.o: $(FAAD_DIR)/libfaad/error.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/filtbank.o: $(FAAD_DIR)/libfaad/filtbank.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/hcr.o: $(FAAD_DIR)/libfaad/hcr.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/huffman.o: $(FAAD_DIR)/libfaad/huffman.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/ic_predict.o: $(FAAD_DIR)/libfaad/ic_predict.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/is.o: $(FAAD_DIR)/libfaad/is.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/lt_predict.o: $(FAAD_DIR)/libfaad/lt_predict.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/mdct.o: $(FAAD_DIR)/libfaad/mdct.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/mp4.o: $(FAAD_DIR)/libfaad/mp4.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/ms.o: $(FAAD_DIR)/libfaad/ms.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/output.o: $(FAAD_DIR)/libfaad/output.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/pns.o: $(FAAD_DIR)/libfaad/pns.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/ps_dec.o: $(FAAD_DIR)/libfaad/ps_dec.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/ps_syntax.o: $(FAAD_DIR)/libfaad/ps_syntax.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/pulse.o: $(FAAD_DIR)/libfaad/pulse.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/rvlc.o: $(FAAD_DIR)/libfaad/rvlc.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_dct.o: $(FAAD_DIR)/libfaad/sbr_dct.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_dec.o: $(FAAD_DIR)/libfaad/sbr_dec.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_e_nf.o: $(FAAD_DIR)/libfaad/sbr_e_nf.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_fbt.o: $(FAAD_DIR)/libfaad/sbr_fbt.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_hfadj.o: $(FAAD_DIR)/libfaad/sbr_hfadj.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_hfgen.o: $(FAAD_DIR)/libfaad/sbr_hfgen.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_huff.o: $(FAAD_DIR)/libfaad/sbr_huff.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_qmf.o: $(FAAD_DIR)/libfaad/sbr_qmf.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_syntax.o: $(FAAD_DIR)/libfaad/sbr_syntax.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/sbr_tf_grid.o: $(FAAD_DIR)/libfaad/sbr_tf_grid.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/specrec.o: $(FAAD_DIR)/libfaad/specrec.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/ssr.o: $(FAAD_DIR)/libfaad/ssr.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/ssr_fb.o: $(FAAD_DIR)/libfaad/ssr_fb.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/ssr_ipqf.o: $(FAAD_DIR)/libfaad/ssr_ipqf.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/syntax.o: $(FAAD_DIR)/libfaad/syntax.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@
$(FAAD_DIR)/libfaad/tns.o: $(FAAD_DIR)/libfaad/tns.c
	psp-gcc $(FAAD_CFLAGS) -c $< -o $@

$(FAAD_LIB): $(FAAD_OBJS)
	psp-ar rcs $@ $^

$(TARGET).elf: $(FAAD_LIB)

include $(PSPSDK)/lib/build.mak

# Note: -lpspuser is already added by build.mak, no need to duplicate

-include $(OBJS:.o=.d)

$(RELEASE_DIR)/EBOOT.PBP: $(TARGET).elf PARAM.SFO $(PBP_ICON) $(RELEASE_TOKEN)
	@mkdir -p $(RELEASE_DIR)
	@psp-strip $(TARGET).elf -o $(RELEASE_DIR)/$(TARGET)_strip.elf
	@rm -f $@
	@pack-pbp $@ PARAM.SFO $(PBP_ICON) NULL NULL NULL NULL $(RELEASE_DIR)/$(TARGET)_strip.elf NULL
	@rm -f $(RELEASE_DIR)/$(TARGET)_strip.elf

$(RELEASE_TOKEN):
	@mkdir -p $(RELEASE_DIR)/config
	@cp config/token.example.txt $@

$(RELEASE_DIR)/fonts/%: fonts/%
	@mkdir -p $(RELEASE_DIR)/fonts
	@cp $< $@

$(RELEASE_DIR)/assets/%: assets/%
	@mkdir -p $(dir $@)
	@cp $< $@

# Rebuild everything
.PHONY: rebuild
rebuild: clean all
