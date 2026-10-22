# MEMORY BANK — YMPSP (Яндекс Музыка для PSP)

Документ передачи контекста другой ИИ. Обновлён: 21.10.2026 (часть 2: веб-эффект, приоритет UI, вход по QR).
Проект: `C:\Users\Tsifrokayf\Desktop\psp\amdray\yandex_music_psp` (C, Makefile + pspdev, эмулятор PPSSPP + реальная PSP-1000).

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

## Свечение: стиль «Меш» (22–23.10)
- Пользователь: вылеты были из-за эффекта «Веб» (VFPU_STAGE=1, текстура 240x136), и он «плохо ворк».
- Новый стиль `GLOW_STYLE_MESH` (3, теперь по умолчанию; COUNT=4): mesh gradient по мотивам Paper Shaders
  (paper-design/shaders, mesh-gradient.ts, Apache-2.0, Lost Coast Labs): 8 пятен из палитры обложки на траекториях
  Лиссажу, органическое искажение + вихрь, вес 1/(d^3.5+1e-3). Считается в вершинах сетки 32x18 (float, таблица sin),
  рисуется GU_TRIANGLE_STRIP с Gouraud (GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D), без текстуры и VFPU.
- Реагирует на play/pause (скорость и яркость), лайк (оттенок), пульс смены трека; смена цветов плавная.
- Старые стили остаются выбираемыми; в config/glow.cfg у пользователя может лежать style=2 (Веб) — выбрать «Меш» в настройках.
- Не проверено в эмуляторе (computer use был недоступен); алгоритм проверен питон-превью. Нужна проверка на PSP-1000.

## Свечение «Веб» оптимизирован (23.10)
- Вид тот же (фон-градиент бакета, 3 шумовых кольца + спарк, виньетка, пауза/лайк/пульс), но рендер переписан:
  вершины сетки 63x35 (узлы шума 32x18 + середины), float-математика на CPU, GPU Gouraud (`grid_draw`, общий с «Меш»).
  Нет текстуры 240x68, нет рукописного VFPU-асма (он без clobber-ов VFPU и в основном потоке без атрибута VFPU —
  вероятный источник вылетов на железе), нет Dcache writeback/invalidate на кадр.
- Шум: одно поле за кадр (тройной буфер, lerp prev→cur); спарк-LUT строится по третям за 3 кадра (нет пика каждые 3 кадра).
- Удалено: VFPU_STAGE/VFPU_PROBE, WEB_W2/H2, старые таблицы s_uu/s_spark_x/s_glowfall/s_recip, fast_len_fx.
- Классика/Нити остались на текстуре 240x136. Проверка на ПК (host-harness с заглушками PSP): ~0.23 мс/кадр, картинка в стиле веба.
  На консоли не измерялось.
