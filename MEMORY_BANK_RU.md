# MEMORY BANK — YMPSP (Яндекс Музыка для PSP)

Документ передачи контекста другой ИИ. Обновлён: 23.10.2026 (см. раздел «СОСТОЯНИЕ НА 23.10.2026 — читать первым»).
Проект: `C:\Users\Tsifrokayf\Desktop\psp\amdray\yandex_music_psp` (C, Makefile + pspdev, эмулятор PPSSPP + реальная PSP-1000).

---

## СОСТОЯНИЕ НА 23.10.2026 — ЧИТАТЬ ПЕРВЫМ (итог сессий 21–23.10, Claude)
**Как собирать.** Облако (Claude) собирало `make -j4 ympsp.elf` + `psp-strip` + `pack-pbp PARAM.SFO branding/ICON0.PNG NULL NULL NULL NULL x.elf NULL`
(обычный `make` на облаке падает на правиле токена). У пользователя — MSYS2/pspdev, как в разделе 1. **Makefile на ПК инструменты
Claude записать не могут (защищён)**: новые модули лежат в `Makefile.new` — заменить: `Move-Item -Force Makefile.new Makefile`
(добавлено в OBJS: `ui_viz.o`, `ui_idle.o`, `ui_theme.o`, `net_speed.o`). Без этого линковка упадёт на новых файлах.
**Git.** Репозиторий в папке проекта, текущая ветка `exp-flac` (последний коммит от 22.10: «Heap 16->12MB…»). Всё ниже — НЕ закоммичено.
`.gitignore` игнорирует `*.PBP, *.elf, release/, debug_tools/, Makefile.new` — EBOOT и Makefile.new в гит не попадут.
**Что нового относительно последнего коммита** (файлы, которые нужно добавить в коммит):
- Новые: `src/services/net_speed.c`, `include/services/net_speed.h`, `src/ui/ui_viz.c`, `include/ui/ui_viz.h`, `src/ui/ui_idle.c`, `include/ui/ui_idle.h`,
  `src/ui/ui_theme.c`, `include/ui/ui_theme.h`, `fonts/ui_gri.cbmf, ui16_gri.cbmf, ui_pdp.cbmf, ui16_pdp.cbmf, ui_cov.cbmf, ui16_cov.cbmf`.
- Изменённые: `Makefile` (после замены), `src/ui/ui_glow_bg.c`, `include/ui/ui_glow_bg.h`, `src/ui/ui_draw.c`, `include/ui/ui_draw.h`, `src/fonts/text.c`,
  `include/fonts/text.h`, `src/fonts/cbmf_fonts.c`, `include/fonts/cbmf_fonts.h`, `src/ui/ui_icon_atlas.c`, `src/ui/ui_screen_menu.c`,
  `src/ui/ui_screen_now_playing.c`, `src/ui/ui_screens.c`, `src/core/logger.c`, `include/core/logger.h`, `src/services/audio_player.c`,
  `include/services/audio_player.h`, `src/services/audio_cache.c`, `src/services/ym_api_download.c`, `include/services/ym_api.h`,
  `src/services/locale.c`, `include/services/locale.h`, `src/core/fs.c`, `include/core/fs.h`, `src/main.c`, `src/services/net_http.c`,
  `src/services/net_tls.c`, `src/services/net_client.c`, `src/services/eq.c`, `src/services/last_play.c`, `src/services/ym_aac.c`,
  `src/services/ym_flac.c`, `include/services/ym_api_types.h`, `MEMORY_BANK_RU.md`.
**Функции, добавленные за эти сессии** (подробности — в разделах ниже по датам):
1. Свечение: «Веб» (порт шейдера Яндекса, кэш шума 64x36, адаптивный LOD 120x68/88x50/60x34), Классика на сетке, Нити на текстуре, Winamp-стили
   (Спектр/Осциллограф/Круг), подложка и тень под текстом. 2. Настройки: Логи вкл/выкл, качество Авто (по скорости) и FLAC (тест), Тема
   (Стандарт/ГРИ/PDP/Цвета обложки), Бездействие (+время), список настроек с прокруткой. 3. Кнопка SELECT — быстрый вход в настройки
   с любого экрана (кроме заставки, входа по коду и поиска), повторно — закрыть. 4. Режим бездействия на экране воспроизведения
   (полупрозрачно / скрыть / обложка на весь экран). 5. Темы: шрифты CBMF + перекраска UI (`ui_theme_map`).
**Что НЕ проверено.** Консоль (карта F: не была подключена), а также в PPSSPP: Select, бездействие, темы/шрифты, Круг/Осциллограф на реальной PSP,
LOD Веба на PSP-1000 (в эмуляторе падает на 60x34). Проверено в PPSSPP: логи вкл/выкл, качество, Веб, Классика, Нити, Спектр/Осциллограф/Круг,
подложка текста.
**Нюанс по логгеру.** `src/services/library.c` (новее моей копии, правки другого ИИ) вызывает `logger_checkpoint(fmt, ...)`; в логгере, который
у меня был, её не было — я добавил: пишет строку `checkpoint: …` и сразу `logger_flush_now()`. Если у другого ИИ была своя реализация — сравнить.
**Шрифты тем нужно класть в `fonts/` рядом с EBOOT** (иначе тема только перекрашивает UI). Генератор шрифтов — скрипт на Python (DejaVu → CBMF v2),
исходник скрипта лежит в облачной сессии (`/tmp/.../fontgen/gen.py`), в репозиторий не добавлен; при необходимости — формат CBMF описан в `include/fonts/cbmf_file.h`.
**Токены.** Токены, присланные в чат, не коммитить и не использовать; `config/token.txt` в `.gitignore`.

---

## 1. Сборка, деплой, тестирование (Windows, PowerShell)

**Никаких heredocs в PowerShell.** Скрипты пишутся в `%TEMP%\opencode\*.ps1` и запускаются оттуда.

**Сборка:**
```powershell
& "C:\tools\msys64\usr\bin\bash.exe" -lc 'export PATH=/c/pspdev/bin:$PATH; cd /c/Users/Tsifrokayf/Desktop/psp/amdray/yandex_music_psp; make -j4 > /c/Users/Tsifrokayf/AppData/Local/Temp/opencode/build.log 2>&1; echo EXIT=$?'
```
Затем `Select-String -Path "$env:TEMP\opencode\build.log" -Pattern "warning:|error:"` — нужно чисто, EXIT=0.

**Деплой в эмулятор:**
1. `Get-Process PPSSPPWindows64 | Stop-Process -Force`
2. Копия `release\EBOOT.PBP` → `C:\Users\Tsifrokayf\Documents\PPSSPP\PSP\GAME\YMAMD\EBOOT.PBP`
3. Ротация логов: `data\logs\app.log` → `app.log.prevN` (максимум prev16; эмулятор также может писать на `F:\psp\GAME\yandex_music_psp\data\logs\` — реальная PSP)
4. Запуск: `Start-Process "G:\PPSSPP\PPSSPPWindows64.exe" -ArgumentList 'ms0:/PSP/GAME/YMAMD/EBOOT.PBP'`
5. Если лога нет через ~14с — `%TEMP%\opencode\tile.ps1` (всплывает окно) и ждать ещё 14с. Таймаут команд с эмулятором ≥90с.

**Автоматизация ввода (скрипты в `%TEMP%\opencode\`):**
- `sendkeys.ps1 -Keys 0x58,0x28,...` — шлёт клавиши, фокусирует PPSSPP внутри
- `ppshot.ps1 <tag>` — скриншот в `%TEMP%\opencode\<tag>.png` (его потом прочитать Read-ом и посмотреть!)
- `tile.ps1` — поднять окно эмулятора
- Дефолтные клавиши PPSSPP: CROSS=Z(0x5A), CIRCLE=X(0x58), SQUARE=A, TRIANGLE=S, START=Space(0x20), SELECT=V(0x56), D-Pad=стрелки 0x25-0x28 (0x28=DOWN)
- В приложении: CIRCLE(X)=назад/поп-экрана, CROSS(Z)=выбор, SQUARE(A)=стоп, START=пауза, LEFT/RIGHT=пред./след. трек (холд = перемотка)
- Всегда проверять результат скриншотом/логом, а не на веру

**Логи:** `C:\Users\Tsifrokayf\Documents\PPSSPP\PSP\GAME\YMAMD\data\logs\app.log`
Ключевые маркеры: `ap: state -> PLAYING/FINISHED`, `ym_flac:`, `ac:` (audio_cache), `speedtest:`, `pb:` (playback_controller), `mem: heap`.

**Delegator (обязательно перед финальным ответом с кодом/причиной):**
```powershell
# задача в $env:TEMP\dg-task.txt, черновик ответа в $env:TEMP\dg-draft.md (UTF-8 с BOM)
& "C:\Users\Tsifrokayf\AppData\Local\Programs\Delegator\runtime\ai-delegate.cmd" improve -PromptFile "$env:TEMP\dg-task.txt" -DraftFile "$env:TEMP\dg-draft.md"
```
EXIT=0 → отправить исправленный ответ; EXIT=3 → свой ответ без изменений; EXIT=1/другое → свой ответ, ничего не упоминать.

**Ответы пользователю — на русском.**

---

## 2. Ключевые факты об окружении

- PSP-1000 реальная: скорость Wi-Fi 200-550 КБ/с (тест пользователя: FLAC 128КБ/с avg, 141 peak — на грани, FLAC порог ~131 КБ/с). Эмулятор: speed test avg≈1357/peak≈1779 КБ/с (норма; было 312 — см. инцидент в §5).
- Память: главная куча ~16МБ (`mem: heap cap=16777216`), живёт ~1.3-1.9МБ. `memalign(64, ...)` + `free` из `<malloc.h>`.
- Конфиг качества: `config/eq.cfg` (preset/custom/**quality=hq**); `want_mp3=1` если выбран MP3 (сервер отдаёт mp3 192КБ/с).
- ym_aac.c содержит windows-1251 мусор в комментариях — правки только по ASCII-контексту.
- API: `SceLwMutexWorkarea`, `sceKernelCreateLwMutex(&m, "name", 0, 0, NULL)`, Lock/Unlock(&m, 1, NULL); `fs_read(SceUID, void*, size_t)→int`, `fs_lseek→SceOff`, PSP_SEEK_SET, PSP_O_RDONLY; потоки: `sceKernelCreateThread(name, fn, prio, stack, PSP_THREAD_ATTR_USER, NULL)` + `sceKernelStartThread(tid, 0, NULL)`.
- Приоритеты потоков (меньше = выше): audio_worker 0x10, callback 0x11, download/login/wave 0x18, net_supervisor 0x19, **flac read-ahead 0x1C**, speedtest 0x18, **сетевой стек PSP callout/netintr 42=0x2A** (`s_default_config` в net_stack.c), apctl 48=0x30, main (UI) — **дефолт 0x20** (НЕ понижать, см. §5b), **сетевой стек callout/netintr = 0x1F** (выше main; net_client.c + net_stack.c).

---

## 3. Архитектура (src/)

**Сервисы (services/):**
- `audio_player.c` (~2200 строк) — воркер декодирования: mp3/mp4-flac/aac → PCM 44100/stereo → вывод. Форматы: `.mp4` → FLAC-in-MP4 (ym_flac), `.aac` → faad2 (ym_aac), иначе mp3 (minimp3). Есть seek-ожидание (`wait_for_more_data`, ~:573), прогрессивное воспроизведение по префиксу файла.
- `ym_flac.c/h` — FLAC-декодер на dr_flac (single instance `s_flac`), режимы: ram/file/mp4. **см. §5 — недавно переписан**.
- `ym_aac.c/h` — AAC (faad2), снабжается тем же avail/wait контрактом.
- `audio_cache.c` — слоты кэша музыки (`cache_music/<id>.mp4|.mp3|.aac`), воркеры докачки, `ac_trim_music` (evict с keep-списком: слоты + pending_jobs + **hint_next** + extra_id), `audio_cache_hint_next()` (мьютекс-guarded), статус/прогресс для плеера (`audio_cache_get_status`: downloaded_bytes/content_length/complete/progressive).
- `playback_controller.c` — очередь, `start_track` (устанавливает hint=следующий трек из очереди после проверки available), авто-переход, префетч (gate по прогрессу), ошибка→скип (PB_MAX_CONSECUTIVE_ERROR_SKIPS).
- `net_tls.c` — мьютекс-guarded TLS (mbedtls) поверх нетворк-слоя PSP; **недавно правки в hot-path** (см. §5).
- `net_http.c` — синхронные GET + `http_stream` (chunked, identity, Range); read_buf 8192; задержки WANT 1мс.
- `speed_test.c` — «Настройки → Тест скорости: X: запустить»; качает реальный CDN-файл 256КБ/с-варианта `get_aac_raw` для текущего/кэшированного трека; пишет `speedtest: done avg=.. peak=.. bytes=.. ms=.. rc=`; rc=0 = файл докачан целиком, rc=-2 = 10с лимит.
- `eq.c` (persist в eq.cfg), `locale.c` (ru/en), `ym_api.*` (API Я.Музыки, качество/URL), `mp4_track.c` (FLAC-in-MP4), `token_loader`, `net_stack`, `cover_cache.c` (обложки с disk keep=64).

**UI (ui/):** главное меню (`ui_screen_menu.c`: Сейчас играет/Альбомы/Плейлисты/Исполнители/Моя волна/Поиск/Настройки), Настройки (Аккаунт/Сеть/Тест скорости/Помощь/Эквалайзер/Качество/Автозапуск/Свечение/Память), Now Playing, списки, поиск. Навигация: CIRCLE=поп, CROSS=выбор. Speed test: из меню → Настройки (DOWN×6, Z) → Тест скорости (DOWN×2, Z).

**Ядро (core/):** fs (VFS ms0:/ef0:, evict LRU `fs_evict_except`), logger (logLine + logger_flush), input, mem.

---

## 4. История фаз (всё сделано и проверено до 21.10)

1. Двукратный счёт прогресса FLAC (flac_file_progress), 416-restart, порядок аргументов logLine — исправлено.
2. `fs_evict_except` (не выгружать файлы активного слота), `ac_trim_music` trim-до-джоба.
3. Качество mp3: `want_mp3`, mp3→«nq» на сервере, 192КБ/с подтверждён.
4. Префетч gate по complete, COVER_DISK_KEEP=64.
5. `wait_for_more_data` complete-retry (ждать только сдвиг счётчика, не +64K).
6. **audio_cache_hint_next** — при старте трека ставится hint следующего; keep-list trim = 5 id. Проверено: переход на prev-трек не эвиктит target, префетч → `cache hit slot=1`.
7. **FLAC stall/freeze фикс (главное этой сессии, см. §5)**.
8. Сетевые микро-оптимизации (§5, включая откат SO_RCVBUF).
9. **Веб-свечение + приоритет UI + вход по QR через Паспорт + фиксы порчи кучи (read-ahead), newlib-гонки, зависания OSK** (§5b, проверено в PPSSPP).

---

## 5. Сессия 21.10.2026: FLAC зависания + сеть (ГОТОВО, проверено)

### Симптомы пользователя
- FLAC-трек ждёт ~27с (полная докачка) и сам переключается на следующий.
- Возврат к нему — играет; иногда фриз при быстром переключении. Баг с FLAC.
- Реальная PSP: 127avg/141peak КБ/с — на грани FLAC-порога, нужен запас.
- «Целиковый FLAC тоже иногда фризит — добавь буфер».

### Корневые причины
1. Фронтер докачки: `ym_flac_mp4_read` возвращал 0 когда `c_off + c_size > dl` → dr_flac фиксировал EOF навсегда → audio_player видел got==0 + complete → «eof decode terminal» → FINISHED → автоскип.
2. На целиковом файле: dr_flac без SEEKTABLE (заголовок 42 байта), resync = переоткрытие + бинарный/brute-force seek кадров — секунды на PSP = фриз. Плюс возможный ядовитый EOF-снапшот.

### Реализовано (все правки в рабочем дереве, билд чистый EXIT=0)
**ym_flac.h/c:**
- Тип `YmFlacWaitFn` (`int (*)(void *user, int64_t need_bytes)`; 0=ок, -1=прервать), `ym_flac_set_wait()`, `ym_flac_at_end()` (decl в h).
- `ym_flac_open_file(int fd, const char *path, ...)` — новая сигнатура (path нужен для read-ahead); открытые fd вручную **не закрываются** — владелец audio_player.
- В структуре: wait/wait_user; read-ahead поля (ra_fd отдельный fd, ra_buf 512КБ выровненный, ra_base/ra_have/ra_want, ra_stop, ra_thread, ra_mu `SceLwMutexWorkarea`, ra_ok); макросы `YM_FLAC_RA_SIZE (512*1024)`, `YM_FLAC_RA_CHUNK (64*1024)`.
- RA-хелперы: `ra_try_read` (из окна, -1=не покрыто), `ra_direct_read` (fs_lseek+fs_read по f->fd — fallback), `ra_thread_main` (чанк в цикле: clamp до avail_fn, memalign, чтение ra_fd, копия под мьютексом), `ra_start` (поток `flac_ra`, prio 0x1C, stack 0x4000, PSP_THREAD_ATTR_USER; лог `ym_flac: read-ahead on '%s' window=512K`), `ra_stop_and_free` (вызывается в `ym_flac_close`, до drflac_close).
- `ym_flac_on_read`: `pos >= avail` → wait(pos+1); иначе ra_try_read → при -1 ra_direct_read.
- `ym_flac_mp4_read`: на фронтере `c_off + c_size > dl` → wait(c_off+c_size) → ra_try_read → fs fallback.
- После `memset` в каждом open_*: `ra_fd = -1; ra_thread = -1;` ra_start только после успешного open_common (в open_file и open_mp4).
- `ym_flac_at_end`: 1 если `!dec || currentPCMFrame + 8192 >= total_frames` (маржа ~186мс).

**ym_aac.h/c:** `ym_aac_at_end(a)` → `next_sample >= total_samples`.

**audio_player.c:**
- `flac_wait_cb(user, need)` = `wait_for_more_data(id, need)==0 ? 0 : -1`; вызывается после open: `ym_flac_set_wait(flac, flac_wait_cb, (void*)track->id)`.
- Вызов `ym_flac_open_file(fd, source->path, ...)`.
- Готовый-но-непрочитанный гейт: `if (!status.complete) wait(downloaded_bytes)...; else if (ym_flac_at_end) terminal; else flac_stuck++ (≤5 ретраев, лог "resync retry N") → 5 → ERR_SOURCE`. Счётчик сбрасывается при got>0. Аналогично в AAC-ветке (`aac_stuck`).

**net_tls.c:** WANT_READ задержка 5мс→1мс (~:846); WANT_WRITE 2мс→1мс (~:786); убран `logger_flush()` из лога send (~:756). **НЕ СТАВИТЬ SO_RCVBUF/SO_SNDBUF** — см. инцидент.

**net_http.c:** 4 задержки WANT 5мс→1мс; оба `read_buf[4096]` → `[8192]`.

### Инцидент: SO_RCVBUF=32KB (важный опыт!)
Поставил SO_RCVBUF=32768/SO_SNDBUF=16384 после SO_NONBLOCK → speed test упал 1357→312 КБ/с (rc=-2, файл не докачивался за 10с). Причина: явная установка SO_RCVBUF на Windows **выключает авто-подстройку TCP-окна**. Убрал оба — скорость вернулась (avg=1357/peak=1779, rc=0). **Вывод: не трогать размеры сокетных буферов.**

### Верификация (PPSSPP, 21.10)
- Стриминг с удалённого файла (удалил `79341391.mp4`, перезапуск): download job → progressive ready 269КБ → `ym_flac: read-ahead on window=512K` → PLAYING → 56с непрерывного воспроизведения **без единого wait/resync/short-read/stuck** → докачка 18.6МБ завершена (~323КБ/с, `dl speed` каждые 5с стабильно) → трек отыгран 149.3с целиком → `resync ok target=6586637 got=0` (1 раз у самого конца, безвредно) → `eof decode terminal played_ms=149356` (≈полная длительность 149.4с) → FINISHED. Ложного скипа нет.
- `pb: queue end` после FINISHED = очередь в сессии из 1 трека (не баг).
- Speed test после отката: avg=1357/peak=1779 KB/s rc=0 (базлайн до правок 1331/1788 — совпадает).
- Heap: used=1.87МБ free=14.9МБ. Ошибок в логе нет (кроме безвредного boot `hal_input: kernel controller read failed ... NOTE unavailable`).
- AAC путь и wait-путь у фронтера в эмуляторе НЕ триггерились (сеть быстрая) — см. TODO.

---

## 5b. Сессия 21.10.2026 (часть 2): веб-эффект грузит консоль, FLAC буферится; вход по QR

Сделано в облачной среде (pspdev-тулчейн latest, `make ympsp.elf`, -Wall -Wextra -Wshadow: **0 warnings, EXIT=0**). Проверено в PPSSPP (см. ниже); на реальной PSP — ещё нет.

### Проверка в PPSSPP (21.10, после правок) и найденные старые баги
Деплой через device_commit_files в `Documents\PPSSPP\PSP\GAME\YMAMD\EBOOT.PBP` (бэкап старого: `EBOOT.PBP.bak`, лог: `data\logs\app.log.prev18`), запуск через «Открыть...» в PPSSPP. Клавиши в PPSSPP жать удержанием ~100 мс (короткие нажатия игра не ловит).

1. **Вылет при входе в «Сейчас играет» на FLAC из кэша — был и в сборке пользователя из `release/`** (не регрессия этой сессии): Invalid Memory Access в `_Balloc` (из `snprintf("%.1f kHz")`), потом в `mallinfo` (mem_probe). Причина: **переполнение буфера read-ahead** в `ym_flac.c` `ra_thread_main`: кусок 64К не ограничивался свободным местом окна → `fs_read(ra_buf+have, 64K)` писал до 64К за конец 512К-буфера → порча кучи. Тот же код скопирован в `ym_aac.c`. Фикс в обоих: `if (chunk > RA_SIZE - have) chunk = RA_SIZE - have;` (в ym_aac.c правка побайтовая, комментарий ASCII — кодировка 1251 сохранена).
2. **newlib на PSP не потокобезопасен для strtod/dtoa** (один `_impure_ptr` на все потоки, кэш `_Balloc` без локов). Новый `include/services/libc_guard.h` (реализация в конце `api_parser.c`): `ym_libc_guard_init()` (в main до потоков), `ym_json_parse[_len]` (все `cJSON_Parse*` заменены), `ym_strtod` (ui_layout), `ym_fmt_fixed`. **Правило: в printf-семействе НЕ использовать %f/%g/%e** — убраны все (now_playing kHz, eq kHz/лог, rotor %.3f, id %.0f → %lld).
3. **Поиск зависал навсегда после закрытия экранной клавиатуры** (0 FPS, лог `osk: still in dialog st=3`): в `osk_input.c` при `PSP_UTILITY_DIALOG_QUIT` не вызывался `sceUtilityOskShutdownStart()`. Исправлено (Update только в VISIBLE, Shutdown один раз в QUIT).

Результаты: FLAC из кэша играет полностью без вылета; стриминг (AAC 8.6 МБ) на экране с веб-эффектом — `dl speed 858 KB/s`, без буферинга, куча стабильна (~1.9–2.0 МБ); **вход по QR проверен целиком** с телефона пользователя: `qr: confirmed on phone` → `get_session cookies=16 sid=1` → `xtoken ok` → `music token ok` → `login: user info rc=0` → токен сохранён → после перезапуска авторизация по нему ОК. Пока не подтверждено, status отвечает `{}` (2 байта). Поиск: клавиатура закрывается, экран «(пусто)».
Тулчейн облачной проверки: pspdev release latest (gcc 15.2.0, как у пользователя), 0 warnings.

### Реальная PSP-1000 (21.10, вечер): старт 100+ с, меню «через раз», выключение на плейлисте
- Лог консоли (`F:\psp\GAME\yandex_music_psp\data\logs\app.log.prev2` и следующий): **каждая запись на Memory Stick ~2.1 с** (кратно), CPU idle 99%. Запись 16 КБ картинки заставки — 15 с, дамп account_status — 8.7 с. В логе 4.10 (app.log.prev1) те же шаги — миллисекунды. Приоритет main тут ни при чём (с 0x20 то же самое). Вероятно, медленная запись самой карты (FAT32: заполненность/фрагментация, мак-мусор .Spotlight-V100/.fseventsd) — проверить свободное место.
- Почему это морозило UI: логгер писал синхронно и ДЕРЖАЛ мьютекс логгера во время fs_write; main вызывал logger_flush каждые 250 мс вне воспроизведения; cpu.txt писался из UI-потока; общий мьютекс fs.c держится на каждом sceIo* (медленная запись блокирует все чтения, включая аудио).
- Исправлено:
  - **Асинхронный логгер** (`core/logger.c`): поток `log_writer` (prio 0x30, период 500 мс), копирует кусок ≤16 КБ под мьютексом и пишет БЕЗ мьютекса; `logger_flush()` только будит писателя; при полном буфере строка теряется (`[dropped N log lines]`), а не блокирует. Во время воспроизведения (realtime) пишет только при заполнении ≥3/4. `logger_shutdown` останавливает поток и делает финальный синхронный сброс. ⚠ При падении теряется несброшенный хвост (см. ниже: sceIoSync раз в 4 с, во время воспроизведения порог 8 КБ).
  - **cpu.txt больше не пишется**: телеметрия CPU идёт в app.log строками `cpu: ...`.
  - **Отладочные дампы `data/logs/*_response.json` выключены** (`YM_DEBUG_RESPONSE_DUMPS 0` в logger.h; net_client/ym_api_playlists/ym_api_albums). Они нигде не читались.
  - **Заставка**: если скачанная картинка совпадает с кэшем (размер+memcmp) — запись stage+cache пропускается (`splash: image unchanged, cache kept`).
- Выключение консоли при открытии плейлиста: в логах момента нет (лог обрывается раньше). В PPSSPP плейлисты и «Мне нравится» (1923 трека) открываются. Ждём лог с консоли на новой сборке.

### Реальная PSP-1000 (21.10, ночь): вылет при загрузке списка треков, запуск всё ещё долгий
Причины найдены по логу консоли (`app.log` после сборки с `sceIoSync`):
- **`track_meta_store.c` писал КАЖДЫЙ трек на карту** (lseek+write 1,2 КБ под fs-мьютексом; файл `data/cache/meta/store.bin`, до 4096 слотов, при загрузке — скан со seek+read на слот). Альбом из 12–16 треков = 12–16 записей по ~2 с; плейлист на сотни/тысячи треков = минуты записей подряд + блокировка всего файлового I/O. Это главный кандидат на «выключение консоли при загрузке списка» (серия записей на медленной/микро-SD-в-адаптере карте может просаживать питание). Переписано: **ОЗУ-первым** — `META_RAM_SLOTS 320` (~380 КБ, LRU), файл читается один раз крупными кусками (64 КБ), запись на карту — фоновым потоком `meta_flush` (prio 0x32) одной крупной записью (до 96 слотов) не чаще раза в 15 с после первой «грязной» записи и целиком при выходе. Формат файла прежний (версия 4), старые файлы читаются (берутся первые 320 слотов).
- **Логгер писал через fs_write под общим fs-мьютексом** → любая запись лога (~2 с) блокировала чтение `player.cfg`, `lastplay.dat` (они читались по 2–4,5 с вместо 20 мс). Теперь лог пишется напрямую `sceIoWrite` (мимо fs-мьютекса), период 2 с, `sceIoSync` не чаще раза в 4 с; во время воспроизведения пишет при накоплении 8 КБ (раньше 3/4 буфера — терялись десятки КБ хвоста). Шумные TLS-строки (`send#`, `hs alive`, `set SO_NONBLOCK`) убраны/прорежены. Добавлены строки `power: callback info=…` (что прошивка сообщила перед выключением) и `sys_free/sys_max` (свободная СИСТЕМНАЯ память) в `mem: heap`.
- **Старт (было ~54 с до меню, из них 7 с — загрузка EBOOT из XMB):** `text_init` 9 с (шрифты читались через newlib fopen/fread; теперь `fs_read` целиком + замер в логе `text: font N read … ms`), перебор Wi-Fi-профилей 1,2,3 (по ~2,8 с до ошибки; теперь последний рабочий хранится в `config/wifi_profile.cfg` и пробуется первым — пишется только при смене), загрузка заставки ~6 с (теперь пропускается, если кэш `splash.png` свежее 7 суток по mtime; при «без изменений» файл переписывается раз в 7 суток, чтобы обновить mtime), `fs_evict_oldest` обложек (листинг+stat каждого файла) теперь раз в 24 скачанные обложки, а не после каждой.
- Куча 16 МБ, main-стек 2048→1024 КБ (парсинг JSON идёт в рабочих потоках). На реальной PSP после старта свободно в системе всего ~1,5 МБ (`tls: hs starting … free=1556224`) — тяжёлые выделения не добавлять, стеки потоков (64–96 КБ) считать.
- ⚠ PPSSPP на машине пользователя УЖЕ в режиме «Модель PSP: PSP-1000» (Настройки → Система). `open_application` для PPSSPP каждый раз запускает НОВЫЙ экземпляр — не звать повторно, закрывать лишние окна (иначе файл EBOOT блокируется и два экземпляра пишут один app.log).

### Причина буферинга при веб-эффекте
- main-поток имел дефолтный приоритет **0x20**, а сетевой стек PSP (`sceNetInit` callout/netintr) — **42 = 0x2A**, т.е. НИЖЕ UI. Пока main считал кадр веб-свечения (VFPU-ядро 240×136 + float-пак + раз в 3 кадра 1728 snoise + 8160 atan2), netintr не получал CPU → приём Wi-Fi стоял → докачка падала → FLAC «буферинг». Воркеры приложения (0x10–0x1C) этим не страдали, страдал именно стек.
- ~~Фикс 1: `PSP_MAIN_THREAD_PRIORITY(0x2C)`~~ — **ОТКАЧЕН**: на реальной PSP-1000 с main=0x2C каждая операция с Memory Stick и переходы splash ждали кратно ~2.1 с при CPU idle ~99% (запуск 7 с → 100+ с, меню «листает через раз»; лог `data/logs/app.log.prev2` на карте). В PPSSPP не воспроизводится.
- Фикс 2 (текущий): main на дефолтном 0x20, а `callout_prio`/`netintr_prio` в `sceNetInit` = **0x1F** (было 42) — сеть вытесняет UI, UI не понижен.

### Оптимизация веб-стиля (`src/ui/ui_glow_bg.c`, VFPU-путь)
- Текстура веба **240×68** (полная ширина, половина высоты, `WEB_VROWS`), строка r берёт исходную строку 2r+1; GPU LINEAR растягивает. Ядро и пак — вдвое меньше работы. `s_tex_rows` = 68 для веба / 136 для classic/strands; UV спрайта и `DcacheWritebackRange` (было 256 КБ/кадр → ~70 КБ) — по `s_tex_rows`.
- Пак: постолбцовая часть `bg*(1-bright)*255 + liked + 0.5` раз за кадр (`s_packBg`), попиксельно mul+add+дизер (`kDith255`). Математика та же.
- Спарк/recip: ядро читает recip ТОЛЬКО из квада 0 строки (`RQX` без смещения по xq — так было и раньше, внешний вид сохранён). Поэтому atan2-геометрия считается разово (`vfpu_init_spark_geom`), recip — 3 числа на строку раз в 3 кадра (`vfpu_build_recip_table`), раскладка `vfpu_expand_recip_row`. Раньше: 136×60 atan2 + 24k делений раз в 3 кадра, а в остальные кадры все строки брали recip строки 135 (несогласованность).
- Шумовые сетки: **тройной буфер** `s_fgrid[3]` (prev/cur/build), по одному полю за кадр (`vfpu_build_grid_field`), lerp `k=(phase+1)/3`. Исправлен баг пилы: было `k = 1 - age/3` (скачок к новой сетке и откат назад каждые 3 кадра).
- C-путь (`VFPU_STAGE 0`) не трогался.

### Вход по QR через Яндекс Паспорт (новое)
- API: `include/services/ya_qr_auth.h`; **реализация — в конце `src/services/ya_auth.c`** (Makefile защищён от записи удалёнными инструментами, поэтому новый .o не добавлялся). `src/services/ya_qr_auth.c` — заглушка-комментарий, в Makefile НЕ добавлять (дубликаты символов). Протокол по AlexxIT/YandexStation `core/yandex_session.py`:
  1. GET `https://passport.yandex.ru/pwl-yandex` → `__CSRF__ = "..."` (страница ~130 КБ) + cookies;
  2. POST `/pwl-yandex/api/passport/auth/password/submit` JSON `{"retpath":"https://passport.yandex.ru/"}` + `X-CSRF-Token` → `track_id` (ответ целиком хранится, это тело шага 4);
  3. POST `/pwl-yandex/api/passport/auth/magic/code` form `location_id=0&magic_track_id=<track>&track_id=` → `link` (рисуется QR);
  4. опрос POST `/pwl-yandex/api/passport/auth/magic/code/status` (тело = JSON шага 2) каждые 2 с до `state=otp_auth_finished`;
  5. POST `/pwl-yandex/api/passport/sessions/get_session` form `track_id=<trackId>` → cookie `Session_id`;
  6. POST `https://mobileproxy.passport.yandex.net/1/bundle/oauth/token_by_sessionid` form client_id/secret приложения «Яндекс» (`c0ebe342...`/`ad0a908f...`), заголовки `Ya-Client-Host: passport.yandex.ru`, `Ya-Client-Cookie: <все cookies>` → `access_token` = x_token;
  7. POST `https://oauth.mobile.yandex.net/1/token` `grant_type=x-token` + client_id/secret Музыки (`YM_CLIENT_ID` из ya_auth.h) → токен Музыки → `net_client_fetch_user_info` → `config/token.txt`.
- Весь флоу проверен в PPSSPP с подтверждением на телефоне (21.10). При сбое смотреть лог `qr:` (там статус и первые 160 байт тела).
- `net_http`: в `HttpRequest` добавлены `extra_headers`, `user_agent`, `no_client_header`, `cookie_jar` (только `http_request`). `HttpCookieJar` (24×(48+768)): приём `Set-Cookie` (удаление по пустому значению/`Max-Age=0`/`1970`), отправка `Cookie:`. С jar/extra заголовок запроса собирается в куче, иначе как раньше на стеке 1536. `path` буфер 256→512. Парсер jar проверен нативным тестом на реальных заголовках Паспорта.
- Экран `ui_screen_device_login.c`: по умолчанию QR Паспорта (QR справа, модуль 2–5 px, ряды тёмных модулей склеены в один rect), автообновление QR каждые 180 с или при протухании (до 6 раз), **Треугольник** — переключение QR ⇄ код (старый device flow), **X** — повтор. Аккаунт без профиля: подсказка «X: войти (QR)». Строки локали `LOCALE_LOGIN_QR_*`, `LOCALE_LOGIN_TO_CODE/QR`; заголовок «Вход в Яндекс».

---

## 6. Известные особенности / ловушки

- **wait-путь FLAC у фронтера не покрыт эмулятором** (там сеть быстрее декодера) — только реальная PSP-1000 с медленным Wi-Fi. Если на консоли появятся скипы — смотреть `flac_wait_cb`/`wait_for_more_data` и логи «buffering»/«resync retry».
- `wait_for_more_data` возвращает 0 при сдвиге счётчика, AUDIO_PLAYER_EOF при complete && required>downloaded, иначе ошибка/стоп. Стоп во время wait: cb вернёт -1 → got==0 → стик-счётчик может увеличиться на 1, но stop_state прервёт цикл раньше ERR_SOURCE — безвредно.
- single-instance: `s_flac` — второй FLAC параллельно невозможен; `s_flac_used` флаг.
- `ym_flac_reopen_dec` (ресинк) НЕ закрывает RA — окно живёт; decoder fd и ra_fd независимы (чтение по разным fd безопасно).
- Очередь/next может быть пустой (`pb: queue end`) — не путать с багом.
- Эвикшн `fs: evicted '<id>.AAC'` при старте нового джоба — штатный trim (keep-список не содержит неподходящих треков).
- PPSSPP навигация может «залипать» (nav gating) — после каждого действия пауза 0.5-2с, проверка скриншотом.
- Кодировка ym_aac.c — windows-1251 в комментариях; не перезаписывать файл целиком.
- **Никаких %f/%g в printf и прямых cJSON_Parse/strtod** — только через libc_guard.h (гонка newlib → порча `_Balloc`).
- **Не понижать приоритет main ниже 0x20.** Сетевой стек — 0x1F (выше main).
- **Никаких синхронных записей на Memory Stick из UI-потока** (на реальной карте запись может стоить секунды). Логи — только через logLine (фоновый писатель).
- Веб-свечение: ядро берёт recip из квада 0 (не по xq). Если «исправить» на по-квадовый — изменится вид колец (появятся угловые выступы спарка); это осознанное решение сохранить прежний вид.
- QR Паспорта: эндпоинты `pwl-yandex/api/...` — внутреннее веб-API, Яндекс может поменять. Резерв — вход по коду (Треугольник).

---

## 7. TODO / следующие шаги

0. **Реальная PSP-1000** (на карте F: сборка от 21.10 ночью с ОЗУ-хранилищем метаданных; прочитать `data/logs/app.log`: строки `text: font`, `power:`, `meta_store: flushed`, `sys_free`): проверить скорость запуска (без пауз ~2.1 с), листание меню, открытие плейлиста «Мне нравится» (в PPSSPP ок, на консоли до фикса — выключение; лог после этого прочитать), веб-эффект + стриминг FLAC, вход по QR.
1. **Реальная PSP-1000**: проверить стриминг FLAC на медленном Wi-Fi (wait-путь, read-ahead, отсутствие скипов/фризов), замерить скорость (settings → Тест скорости; ожидание: рост относительно 127/141 КБ/с за счёт 1мс задержек и 8КБ буфера; реальный рост сети ограничен каналом — фиксы в основном про стабильность).
2. Если на реальной PSP будут скипы при докачке — откат/доработка `flac_wait_cb`, рассмотреть больший RA-буфер (память позволяет: 15МБ свободно).
3. Опционально: покрыть тестом wait-путь эмулятором (искусственно замедлить докачку) — не сделано.
4. Известных открытых багов нет; скорость в эмуляторе в норме.

---

## 8. Полезные команды-шпаргалки

```powershell
# свежие логи
Select-String -Path "C:\Users\Tsifrokayf\Documents\PPSSPP\PSP\GAME\YMAMD\data\logs\app.log" -Pattern "ym_flac|ap: state|speedtest|dl speed|heap" | Select-Object -Last 30 | ForEach-Object Line

# статус кэша музыки
Get-ChildItem "C:\Users\Tsifrokayf\Documents\PPSSPP\PSP\GAME\YMAMD\cache_music" | ForEach-Object { "$($_.Name) $($_.Length)" }

# навигация к speed test: X (pop в меню) → DOWN×6 → Z → DOWN×2 → Z → ждать 14с
& "$env:TEMP\opencode\sendkeys.ps1" -Keys 0x58
& "$env:TEMP\opencode\sendkeys.ps1" -Keys 0x28,0x28,0x28,0x28,0x28,0x28
& "$env:TEMP\opencode\sendkeys.ps1" -Keys 0x5A
& "$env:TEMP\opencode\sendkeys.ps1" -Keys 0x28,0x28
& "$env:TEMP\opencode\sendkeys.ps1" -Keys 0x5A
```

## Свечение «Веб» (23.10, переработано) — Меш УДАЛЁН
- Стиль «Меш» убран по просьбе пользователя (ужасно выглядел): `GLOW_STYLE_COUNT` (сейчас 6, см. раздел «Раунд 23.10 №3»), по умолчанию `GLOW_STYLE_WEB`;
  сохранённый `style=3` отбрасывается при чтении (→ веб).
- «Веб» = порт шейдера music.yandex.ru (сверено с живым воркером v4.1722, запущенным в браузере; исходник шейдера вынут из
  чанка 36095 (base64-воркер)). Рендер: вершины сетки `WG_COLS 88 x WG_ROWS 50` (шаг ~5.5 px), float на CPU, GPU Gouraud
  (`grid_draw`). Нет текстуры/VFPU/Dcache на кадр.
- АДАПТИВНЫЙ LOD: метрика — МИНИМУМ времени `web_update` в окне из 30 кадров (вытеснение потоками TLS/декодера раздувало
  среднее и давало ложные срабатывания), затем EMA; если >9 мс — грубая сетка 44x25 (`s_wg_step/s_wg_draw_step`),
  назад — если с запасом (<6 мс с учётом 2.2x). Время берётся из sceKernelGetSystemTimeWide (в PPSSPP — виртуальное время,
  близко к PSP). В эмуляторе (23.10) LOD=2 включается один раз на ~18 с и держится, 60 fps. В логе: `glow: web lod=2/1`.
- ВАЖНЫЙ БАГ (найден 23.10): `fsin()` в этом файле ТАБЛИЧНЫЙ И НЕПРАВИЛЬНЫЙ (f = a*N/2π, fract, *N — мусор; он в оригинале
  пользователя, Классика/Нити под него «настроены», не трогать). Для веба спарка/сима теперь `sinf` — иначе лучи (spark) были
  в 2–3 раза гуще/длиннее оригинала. Сверка с JS-портом: средний spark 0.04–0.11, f(>0.05)=0.25–0.6.
- Формулы по шейдеру v4.1722: радиус = 0.95-0.15n + 0.3*(2*shared-1); внешний = blob + width/2 + br*(1+max(like,0.6*audio)*50*br),
  br=(1-0.3n)*spark; alpha = 1-smoothstep(lay, lay+edge, |uv*(1-0.5*like)+off|); glow=int/(1+11*dn),
  int=0.15*(1-0.75 sin(2t+off*0.5)) + 0.3*max(like,audio); при лайке к цвету +0.6*like*(1-smoothstep(0.2,0.8*outer,|uv|)).
- ЛАЙК (как в вебе, `likeAnimation`): react колец 0/1/2 = 0.7 в моменты 0/0.10/0.15 с, снова 0 в 0.85/0.95/1.05 с; сглаживание Tt
  600 мс (cur += (tgt-cur)*clamp(elapsed/0.6)). Реализовано в `web_react_step`; слой шума при лайке читается из полей в
  сжатых координатах (`s_wg_sci0` и др.). Запуск: `glow_bg_set_liked` на фронте 0→1, если нет pulse (смена трека) в последние
  120 кадров (веб сравнивает id трека — вспышка только на том же треке).
- ЗВУК (как в вебе `vAudio`): `audio_player_get_levels()` — три полосы (низ ≤250 Гц, середина, верх) от `audio_level_feed()`
  в audio_player.c (однополюсные целочисленные фильтры по готовому PCM, 3 места вывода: flac/aac/mp3). Нормировка в dB в
  `wg_level_norm` (подбиралась на глаз; без аналога FFT). Растёт мгновенно, падает 0.02 за 40 мс; общий множитель за 1 с.
- Host-harness: /tmp (облако) — `glow_host_new.c` + `harness2.c`: ~0.33 мс/кадр на ПК (старый 63x35 был 0.21); на PSP не измерено.
- Старые стили Классика/Нити остались на текстуре 240x136.

## Лог и карта: ленивый логгер (23.10) — ПРИЧИНА подвисаний старта и мигания индикатора
- Лог с реальной PSP-1000 показал: каждая операция с картой занимает секунды (шрифт 11 с, токен 6,5 с, конфиги 4–17 с).
  Причина — сам логгер: запись + `sceIoSync` каждые 2–4 с, а главный цикл звал `logger_flush()` каждые 250 мс; все чтения
  вставали в очередь за записью на Memory Stick, индикатор мигал постоянно.
- `src/core/logger.c`: по умолчанию «ленивый» режим — лог копится в RAM, пишется при 16 КБ (в realtime — при 40 КБ) или раз
  в 60 с; `logger_flush()` в этом режиме НЕ пишет (его зовут сотни мест); `sceIoSync` не чаще раза в 300 с.
  Новое `logger_flush_now()` (в `include/core/logger.h`) — принудительная запись+sync; вызывается из `power_callback` (сон).
  `-DYM_LOG_EAGER=1` — старое поведение (2 с / sync 4 с) для отладки вылетов. Потеря при падении — до минуты хвоста лога.
- Периоды: `cpu:` 5→60 с (`CPU_LOG_US`), `battery:` 30→300 с (`BATTERY_LOG_US`), `mem: heap periodic` 10→120 с (`main.c`).
- `net_stack.c`: из диагностики Wi-Fi убрано лишнее (каждый sceNetApctlGetInfo ~10 мс, ~160 строк на старте).
  Остались: имя/ssid/secure профиля; ssid/strength/channel/ip/gateway активного подключения.
- Проверить на консоли после обновления: исчезло ли мигание индикатора, время чтения шрифтов/токена в `data/logs/app.log`.
- Если шрифт/токен всё ещё читаются секундами при простаивающем логгере — копать дальше (fs-мьютекс, сама карта, состояние
  splash `storage` держалось 11 с).

## PSPLink/PRX отладка (22.10) — справка
- Подробности: `DEBUG_PSPLINK_RU.md` (Windows-native, без WSL). `Makefile.new` — новый Makefile с `BUILD_PRX=1`/`DEBUG=1`
  (оригинальный `Makefile` защищён от записи удалённо — заменить вручную). Отладочные `debug\ympsp.prx|elf` и `debug\tools\`
  (usbhostfs_pc/pspsh) — локальные, в репозиторий не коммитить.
- Пользователь говорит: вылеты при загрузке списков были из-за эффекта «Веб» (старый VFPU-путь), см. раздел про «Веб» выше.

## TODO для следующего ИИ: ПУШ + РЕЛИЗ (не сделано: у Claude нет доступа к GitHub)
- Репозиторий: https://github.com/amdray/yandex_music_psp (пользователь Tsifrokayf). Токены в чат присылались — НЕ использовать
  из чата/памяти; пользователь должен отозвать их. Пуш делать через Git Credential Manager на ПК пользователя.
- Что закоммитить (всё уже лежит в папке проекта `C:\Users\Tsifrokayf\Desktop\psp\amdray\yandex_music_psp`):
  `src/ui/ui_glow_bg.c`, `include/ui/ui_glow_bg.h`, `src/services/locale.c`, `include/services/locale.h` (стиль «Меш», «Веб» на
  сетке вершин); `src/core/logger.c`, `include/core/logger.h`, `src/main.c`, `src/services/system_status.c`,
  `src/services/net_stack.c` (ленивый логгер); ранее: `track_meta_store.c`, `cbmf_fonts.c`, `splash_flow.c`, `cover_manager.c`;
  `MEMORY_BANK_RU.md`, `DEBUG_PSPLINK_RU.md`. НЕ коммитить: `debug\`, `out\`, `*.o`, `ympsp.elf`, токены/`config\TOKEN.TXT`.
- Сообщение коммита (пример): «Свечение: стиль Меш, Веб на сетке вершин GPU; ленивый логгер (нет подвисаний карты)».
- Релиз: `gh release create v<номер> <путь>\EBOOT.PBP --title ... --notes ...`; свежий EBOOT — `F:\psp\GAME\yandex_music_psp\EBOOT.PBP`
  (собран 23.10 с ленивым логгером). Формат архива/номер версии — как у предыдущих релизов (проверить `gh release list`).
- Итоговая проверка перед релизом: сборка `make clean && make` из MSYS2 (облачная сборка делалась `make ympsp.elf` + pack-pbp),
  0 предупреждений, тест на PSP-1000.

## Сеть, FLAC, настройки, иконка (23.10, ещё НЕ проверено на консоли)
- Скорость: тест показывал 129–151 КБ/с — упор в окно TCP (плато ~145 КБ/с при RTT ~100 мс). Сделано: `SO_RCVBUF` 64 КБ
  (в логе `tls: SO_RCVBUF …` — реальное значение), пул sceNetInit 256 КБ, poll в `psp_net_recv`, убраны сны в WANT_READ
  (net_http, рукопожатие). RAM-кольцо в audio_cache (`AC_WBUF_SIZE 192 КБ`) и отдельный поток записи: медленная карта не
  стопорит сокет.
- TLS: кэш сессий (`tls_sess_*` в net_tls.c, 4 хоста, 20 мин) — возобновление вместо полного рукопожатия (~1.2 с на запрос).
  В логе: `tls: resume offered host=…`, `handshake ok … took=… ms`.
- FLAC: lossless только по `encraw` (AES-128-CTR, ключ в ответе get-file-info) и только с Плюсом; без Плюса сервер молча
  отдаёт aac-mp4 (в логе от 22.10: `rejected codec='aac-mp4' want_family=flac`). Реализовано расшифрование (encraw) и попытка
  raw. Негативный кэш: 2 отказа подряд → не пробовать lossless 10 минут (экономит 2–3 рукопожатия на трек).
  Проверять в логе: `direct url ok … transport=encraw` либо `rejected … server_quality=…` (покажет, есть ли Плюс).
- Настройки (качество/свечение/автостарт): запись на карту стоила секунды и блокировала UI. Теперь `fs_write_deferred()`
  (fs.c): фоновый поток, склейка 1.2 с, ≤6 путей по ≤256 Б; `fs_flush_deferred()` — при выходе и в power_callback.
- Иконка лайка: в атласе `assets/ui_icons.t4` запись `in_bookmark` (21x14, лопасти слипались) заменена на `in_heart` 22x20
  (4-битная альфа, чёткая выемка 4 px между лопастями); код: ui_screen_now_playing.c рисует "in_heart". Новый атлас
  положен в assets проекта/F:/YMAMD; запись для старого `in_bookmark` стёрта.
- Просьба пользователя: проверить FLAC, speed test, задержку настроек, мигание сердечка и прислать `data/logs/app.log`.
- 23.10, итоги прогона в PPSSPP и разбор репозиториев (MarshalX/yandex-music-api, acherkashin/yandex-music-open-api, umnik1/yaamp):
  * Заголовок клиента `X-Yandex-Music-Client: YandexMusicDesktopAppWindows/5.95.0` (подпись get-file-info привязана к нему),
    sign = base64(HMAC-SHA256(ключ `kzqU4XhfCaY6B6JTHODeq5`, ts+trackId+quality+codecs без запятых+transport)) без '='.
    Кодеки/транспорты (flac-mp4, flac, aac-mp4, he-aac-mp4, mp3; raw/encraw) выровнены с MarshalX. yaamp (Electron/webamp) и
    open-api новых данных по lossless не дали.
  * Даже с 5.95.0 + encraw/raw сервер на этом аккаунте отвечает `server_quality='nq'`, codec aac-mp4 ⇒ у аккаунта/токена нет
    права на lossless (нужен Плюс/подходящий токен). С клиента не лечится. Негативный кэш: после 1-го отказа lossless не
    пробуется 180 с, после 2-го — 600 с (раньше тратились 2–3 TLS-рукопожатия на трек, в эмуляторе по ~2.4 с каждое).
  * TLS-resume «offered», но время рукопожатия не меняется (~2.4 с эмулятор) — работоспособность НЕ подтверждена.
  * Speedtest в эмуляторе (до 1.7 МБ/с) не показателен — хостовая сеть; замер скорости нужен на консоли.
  * Эмулятор: лайк (треугольник) → сердце белое + вспышка свечения; повторное нажатие — серое; 60 fps с «Веб».

## Раунд 23.10 №3: качество Веб, логи, авто/FLAC-тест, новый рендер, Winamp, читаемость (проверено только в PPSSPP)
Запрос: «поднять качество веб до оригинала; отключение логов; авто-качество по скорости; флак как отдельный тест; остальные
эффекты на новый рендер (сравнив визуал); Winamp-эффекты; текст не виден на фоне эффекта».
- **Логи**: `logger_enabled()/logger_set_enabled()` (logger.c), файл `config/logs.cfg` (`logs=0` — выкл; читается в
  `logger_init` после старта писателя). Строка в меню настроек «Логи: Вкл/Выкл», ВЛЕВО/ВПРАВО. При выключении сначала
  `logger_flush_now()`, в логе остаётся «logger: disabled by settings»; `logLine` при выкл. сразу return. Проверено в PPSSPP.
  Заметка: лог буферизуется, mtime файла обновляется с задержкой ~10 с.
- **Качество музыки**: порядок `lq, nq, hq, auto, mp3, flac`. `auto` — по скорости загрузки: `src/services/net_speed.c`
  (`net_speed_note()` из `ac_log_speed` раз в 2 с, окна <1.5 с игнорируются, EMA alpha 0.35; пороги HQ 90 / NQ 45 КБ/с,
  гистерезис ±15%, без замеров — nq). Строка меню показывает «Авто (Среднее)» и т.п. `ym_api_download_effective_quality()`:
  mp3→nq, flac→hq, auto→по скорости. `flac` — ТЕСТ: обходит негативный кэш, пробует оба транспорта (raw/encraw), при отказе
  падает на hq AAC. `hq` FLAC больше НЕ пробует. В логе: `ac: dl speed … (ema N)`, смена уровня авто.
- **Веб (качество)**: честный ответ — пиксель-в-пиксель не получится (оригинал считается на GPU, у нас CPU-сетка), но
  ближе стали заметно. Измерения хост-харнессом (/tmp/claude-0/work/exp/*.c, cmp.py): главную ошибку давал кэш шума —
  32x18 → 38.7 дБ PSNR, 64x36 → 44.8 дБ. Теперь `NF_W 64 NF_H 36`, пересчёт шума РАЗМАЗАН на `WG_NOISE_P 16` кадров
  (срезами в `web_build_noise_slice`), стоимость кадра ровная. Сетки LOD `kGridLv = {120x68, 88x50, 60x34}`; все уровни дают
  41–48 дБ против эталона, т.е. лучше старой 88x50. Метрика LOD — минимум `web_update` в окне 30 кадров, EMA, бюджет 9 мс,
  гистерезис, при быстром возврате ожидание удваивается. Лог: `glow: web init noise 64x36 took N ms`, `glow: lod=…`.
  В PPSSPP (виртуальное время): init шума 45 мс; со старта уровень 1 → через 1.6 с `lod=2 60x34 (min frame 9.4 ms > 9)` и держится.
  НА КОНСОЛИ НЕ ИЗМЕРЕНО — смотреть `glow: lod=` и fps. Если на PSP-1000 всегда уходит на 60x34, ослабить бюджет (LOD_BUDGET_MS)
  или снизить NF.
- **Классика → сетка**: `classic_update` на сетке, референс `classic_update_tex` (GLOW_REF_TEX) — сходится на 52.5 дБ, дешевле.
  **Нити остались на текстуре 240x136**: сетка давала мозаику (PSNR 26–29 дБ), т.к. они опираются на старый кривой `fsin` и
  тонкие изолинии — не переводить.
- **Winamp-стили** (`GLOW_STYLE_SPECTRUM 3`, `SCOPE 4`, `RADIAL 5`, `COUNT 6`): затемнённая Классика (`s_bg_dim`) + `ui_viz.c`
  (`viz_update/viz_draw`, треугольники GU). Данные: `audio_player_set_analysis(on)`, `audio_player_get_spectrum()` (16 полос
  Goertzel 55…10200 Гц), `audio_player_get_scope()` (64 точки); анализ включён, только пока UI запрашивает (дедлайн 500 мс).
  Проверено в PPSSPP: Спектр, Осциллограф, Круг. Круг центрирован по экрану, левая половина прикрыта обложкой — приемлемо,
  при желании сдвинуть центр вправо.
- **Читаемость текста**: `glow_bg_text_scrim()` — адаптивная тёмная подложка (85-й перцентиль яркости региона → альфа,
  `luma>50 ? 1-50/luma : 0`, зажим 0.16…0.68, для Winamp-стилей минимум 0.62) + тень 1 px через `text_set_shadow()`
  (text.c, проход в `text_draw`), вызовы вокруг текста в ui_screen_now_playing.c (перед нижней панелью тень выключается).
- Новые файлы: `src/services/net_speed.c`, `include/services/net_speed.h`, `src/ui/ui_viz.c`, `include/ui/ui_viz.h`.
  Правки: ui_glow_bg.*, audio_player.*, logger.*, text.*, ym_api_download.c, ym_api.h, audio_cache.c, ui_screen_menu.c,
  ui_screen_now_playing.c, locale.*, Makefile.
- Автоматизация PPSSPP (для следующего ИИ): z=крест, x=круг (назад), s=треугольник; клавиши держать 0.12–0.15 с; Ctrl+W —
  стоп эмуляции. Курсор в Настройках запоминается между заходами — перед нажатием ВПРАВО проверять строку по скриншоту.
  Окно эмулятора ~ [797,360,1357,700]. Доступ computer-use истекает через 30 мин.
- Не сделано: проверка на консоли (карта F: не смонтирована), GitHub push/релиз (см. TODO выше), Scope/Radial на реальном PSP.

## Кнопка SELECT → настройки (23.10, НЕ проверено ни в эмуляторе, ни на консоли)
- `ui_screens_handle_input` (ui_screens.c): по SELECT с любого экрана, кроме заставки, входа по коду и поиска, пушится
  `SCREEN_SETTINGS`; на самих настройках SELECT закрывает их (pop). Круг на настройках работает как раньше. В логе:
  `ui: select -> settings from <экран>` / `ui: select -> close settings`.
- В экран помощи строка про SELECT не добавлена (не трогали раскладку).

## Раунд 23.10 №4: режим бездействия + темы ГРИ/PDP (СОБРАНО, НЕ ПРОВЕРЕНО ни в эмуляторе, ни на консоли)
- **Бездействие** (`ui_idle.c/h`, настройки в `config/idle.cfg`: `mode=N delay=S`). Режимы: Выкл / Полупрозрачно (IDLE_DIM, до 38% яркости)
  / Скрыть (IDLE_HIDE, до 0) / Обложка на весь экран (IDLE_COVER). Задержка 5/10/20/30/60/120 с. `idle_step()` раз в кадр
  в `ui_screen_now_playing_render`: уход в режим ~1.6 с, возврат ~0.25 с; любая кнопка (`input->buttons`), смена трека и
  возврат на экран после паузы >300 мс считаются действием. Прозрачность текста/значков/прямоугольников — глобальный множитель
  `ui_draw_set_alpha()` (ui_draw.c; применяется в `ui_draw_apply_alpha` ← ui_draw_rect, colored_rects, text_draw, значки,
  скрим). Во время бездействия обложка рисуется не `hal_gpu_copy_image`, а текстурой `ui_draw_cover_quad()` (256x256 текстура,
  stride=200, UV в текселях, MODULATE с цветом-альфой). В режиме «на весь экран»: прямоугольник и UV интерполируются от
  200x200 к 480x272 (crop по центру), Ken-Burns (zoom 1..1.07, дрейф), альфа «дышит» 0.45–0.78 поверх живого свечения; текст
  гаснет быстрее. Видео-обложка в режиме бездействия не рисуется (кадры идут), используется статичная обложка.
- **Настройки**: строки «Тема», «Бездействие», «Бездействие через» (ВЛЕВО/ВПРАВО). Строк стало 13 → добавлена прокрутка списка
  в `menu_layout_slot` (видимо ≤ (262−y)/step строк, окно следует за курсором; маркер выбора смещается).
- **Темы** (`ui_theme.c/h`, `config/theme.cfg`: `theme=N`): THEME_DEFAULT / THEME_GRI (янтарный Nixie, тонкие «проволочные» светящиеся
  буквы) / THEME_PDP (красно-оранжевая точечная матрица на чёрном). Перекраска — `ui_theme_map()` в `ui_draw_apply_alpha`
  и `ui_draw_clear`: серая шкала (≈0x20→тон фона, белый→тон текста, с кривой) и акцент #FFD500 заменяются тонами темы; цветные
  (ошибки) и обложка не трогаются. Эффект свечения не перекрашивается (зависит от обложки).
- **Шрифты тем**: `fonts/ui_gri.cbmf, ui16_gri.cbmf, ui_pdp.cbmf, ui16_pdp.cbmf` (CBMF v2, 4 бита покрытия, те же line/baseline: ui 18/11,
  ui16 20/13). Генератор — /tmp/claude-0/work/fontgen/gen.py (DejaVu Sans Condensed → истончение MinFilter + гало gauss → ГРИ;
  DejaVu Sans Mono в mono-режиме FreeType + слабое гало соседей → PDP). Набор: Latin, Latin-1, Latin Ext-A, Greek, Cyrillic, пунктуация (~450 глифов).
  `cbmf_fonts_set_variant(v)` перечитывает оба шрифта в те же атласы (на старте вариант берётся из `ui_theme_get()`); нет файла → обычный шрифт.
  ВАЖНО для консоли: файлы шрифтов нужно положить в `fonts/` рядом с EBOOT (на карте PSP/GAME/YMAMD/fonts/), иначе тема будет только цветной.
- Makefile (защищён от записи через мост): добавить `src/ui/ui_idle.o` и `src/ui/ui_theme.o` в OBJS.
- **Тема «Цвета обложки»** (THEME_COVER=3, шрифт-вариант 3: `ui_cov.cbmf`, `ui16_cov.cbmf` — DejaVu Sans Condensed Bold): `glow_bg_set_cover()` теперь
  отдаёт акценты и средний цвет в `ui_theme_set_cover()`: текст = светлый оттенок 1-го акцента (max→255, +32% белого), акцент = 2-й акцент
  (если близок к 1-му — он же), фон = 9% среднего. `ui_theme_tick()` (из `ui_screens_render`) плавно (7%/кадр) подводит цвета. Палитра берётся при
  открытии экрана воспроизведения (считается один раз на обложку), поэтому до первого захода на него тема нейтральная.
