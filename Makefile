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
       src/ui/ui_screen_search.o \
       src/ui/ui_wave_carousel.o \
       src/ui/ui_glow_bg.o \
       src/ui/glow/grid.o \
       src/ui/glow/classic.o \
       src/ui/glow/strands.o \
       src/ui/glow/web.o \
       src/ui/glow/visualizers.o \
       src/ui/ui_idle.o \
       src/ui/ui_theme.o \
       src/ui/ui_screen_help.o \
       src/ui/ui_screen_memory.o \
       src/ui/ui_screen_eq.o \
       src/hal/hal_input.o \
       src/hal/hal_gpu.o \
       src/core/fs.o \
       src/core/io_profile.o \
       src/core/logger.o \
       src/core/time_format.o \
       src/core/libc_runtime.o \
       src/core/json.o \
       src/core/mem_probe.o \
       src/fonts/cbmf.o \
       src/fonts/cbmf_psp.o \
       src/fonts/cbmf_fonts.o \
       src/fonts/text.o \
       src/services/image_loader.o \
       src/services/locale.o \
       src/services/token_loader.o \
       src/services/ya_device_auth.o \
       src/services/ya_qr_auth.o \
       src/services/last_play.o \
       src/services/track_like.o \
       src/services/ym_api_reactions.o \
       src/services/ym_api_genres.o \
       src/services/ym_api_albums.o \
       src/services/ym_api_rotor.o \
       src/services/ym_api_track_parse.o \
       src/services/ym_api_search.o \
       src/services/osk_input.o \
       src/services/playback_reporter.o \
       src/services/wave.o \
       src/services/album_play.o \
       src/services/eq.o \
       src/services/download_settings.o \
       src/services/screen_idle.o \
       src/services/cpu_clock.o \
       src/core/audio_quality.o \
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
       src/services/ym_mp3.o \
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
       src/services/audio_storage.o \
       src/services/audio_cache.o \
       src/services/audio_player.o \
       src/services/audio_reader.o \
       src/services/audio_analysis.o \
       src/services/playback_queue.o \
       src/services/playback_controller.o \
       src/services/speed_test.o
EXIT_STAGE ?= 0
PSP_CONFIG ?= psp-config
PSPSDK ?= $(shell $(PSP_CONFIG) --pspsdk-path)
RELEASE_DIR = release
FONT_RESOURCES = $(wildcard fonts/*)
ASSET_RESOURCES = $(filter-out assets/ui_layout.txt assets/icon/%.gif assets/icon/%.svg assets/icon/%_preview.png assets/icon/in_explicit.png,$(shell find assets -type f))
PACKAGED_FONT_RESOURCES = $(patsubst fonts/%,$(RELEASE_DIR)/fonts/%,$(FONT_RESOURCES))
PACKAGED_ASSET_RESOURCES = $(patsubst assets/%,$(RELEASE_DIR)/assets/%,$(ASSET_RESOURCES))
RELEASE_TOKEN = $(RELEASE_DIR)/config/token.txt
PBP_ICON = branding/ICON0.PNG

INCDIR = include generated

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
LIBS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto \
    -lpspgu -lpspnet_inet -lpspwlan -lpsputility -lpsppower -lpspaudio -lpspmp3 -lpspaudiocodec \
    -ljpeg -lpng -lz -lcjson -lm

# libcglue (неявный -lc в конце линковки) ссылается на sceUtilityGetSystemParamInt
# из timezone.o; -u втягивает этот стаб в раннюю группу -lpsputility, иначе
# поздняя ссылка открывает вторую группу того же модуля (stubs out of order).
LDFLAGS += -Wl,-u,sceUtilityGetSystemParamInt \
    -Wl,-u,sceUtilityGetSystemParamString \
    -Wl,-u,sceNetInetAccept,-u,sceNetInetBind,-u,sceNetInetGetpeername \
    -Wl,-u,sceNetInetGetsockname,-u,sceNetInetListen,-u,sceNetInetSelect,-u,sceNetInetShutdown

include $(PSPSDK)/lib/build.mak

# Generate before compilation, including the first build without dependency files.
$(OBJS): generated/ui_layout_generated.h
generated/ui_layout_generated.h: assets/ui_layout.txt compile_layout.py layout_model.py
	python3 compile_layout.py $< $@

EXTRA_CLEAN += generated/ui_layout_generated.h

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
