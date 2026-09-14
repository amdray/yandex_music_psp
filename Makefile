TARGET = ympsp
BUILD_PRX = 0
OBJS = src/main.o \
       src/app/app_state.o \
       src/ui/ui_screens.o \
       src/ui/ui_draw.o \
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
       src/ui/ui_screen_eq.o \
       src/hal/hal_input.o \
       src/hal/hal_gpu.o \
       src/hal/hal_fb.o \
       src/core/clock.o \
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
       src/services/ym_api_like.o \
       src/services/ym_api_albums.o \
       src/services/ym_api_wave.o \
       src/services/wave.o \
       src/services/album_play.o \
       src/services/eq.o \
       src/services/qrcodegen.o \
       src/services/splash_flow.o \
       src/services/net_stack.o \
       src/services/net_activity.o \
       src/services/net_ui_status.o \
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
       src/services/video_player.o
EXIT_STAGE ?= 0
PSP_CONFIG ?= psp-config
PSPSDK ?= $(shell $(PSP_CONFIG) --pspsdk-path)
RELEASE_DIR = release
FONT_RESOURCES = $(wildcard fonts/*)
ASSET_RESOURCES = $(wildcard assets/*)
PACKAGED_FONT_RESOURCES = $(patsubst fonts/%,$(RELEASE_DIR)/fonts/%,$(FONT_RESOURCES))
PACKAGED_ASSET_RESOURCES = $(patsubst assets/%,$(RELEASE_DIR)/assets/%,$(ASSET_RESOURCES))
RELEASE_TOKEN = $(RELEASE_DIR)/config/token.txt
PBP_ICON = branding/ICON0.PNG

INCDIR = include

CFLAGS = -O2 -G0 -Wall -Wextra -Wshadow -std=c99 -MMD -MP -DEXIT_STAGE=$(EXIT_STAGE)
EXTRA_TARGETS = $(RELEASE_DIR)/EBOOT.PBP $(PACKAGED_FONT_RESOURCES) $(PACKAGED_ASSET_RESOURCES)
PSP_EBOOT_TITLE = YMPSP

# pspdebug/pspdisplay/pspge/pspctrl/pspnet/pspnet_apctl НЕ указывать здесь:
# build.mak добавляет их последними сам; дубль раньше по строке рвёт группы
# импорт-стабов (psp-fixup-imports: "stubs out of order").
LIBS = -lmbedtls -lmbedx509 -lmbedcrypto \
    -lpspgu -lpspnet_inet -lpspwlan -lpsputility -lpsppower -lpspaudio -lpspmp3 \
    -lpspmpeg \
    -ljpeg -lpng -lz -lcjson -lm

# libcglue (неявный -lc в конце линковки) ссылается на sceUtilityGetSystemParamInt
# из timezone.o; -u втягивает этот стаб в раннюю группу -lpsputility, иначе
# поздняя ссылка открывает вторую группу того же модуля (stubs out of order).
LDFLAGS += -Wl,-u,sceUtilityGetSystemParamInt

include $(PSPSDK)/lib/build.mak

# Note: -lpspuser is already added by build.mak, no need to duplicate

-include $(OBJS:.o=.d)

$(RELEASE_DIR)/EBOOT.PBP: $(TARGET).elf PARAM.SFO $(PBP_ICON) $(RELEASE_TOKEN)
	@mkdir -p $(RELEASE_DIR)
	@psp-strip $(TARGET).elf -o $(RELEASE_DIR)/$(TARGET)_strip.elf
	@pack-pbp $@ PARAM.SFO $(PBP_ICON) NULL NULL NULL NULL $(RELEASE_DIR)/$(TARGET)_strip.elf NULL
	@rm -f $(RELEASE_DIR)/$(TARGET)_strip.elf

$(RELEASE_TOKEN):
	@mkdir -p $(RELEASE_DIR)/config
	@cp config/token.example.txt $@

$(RELEASE_DIR)/fonts/%: fonts/%
	@mkdir -p $(RELEASE_DIR)/fonts
	@cp $< $@

$(RELEASE_DIR)/assets/%: assets/%
	@mkdir -p $(RELEASE_DIR)/assets
	@cp $< $@

# Rebuild everything
.PHONY: rebuild
rebuild: clean all
