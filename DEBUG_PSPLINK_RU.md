# Отладка через PSPLINK (по гайду https://pspdev.github.io/debugging.html)

Прошивка пользователя: ARK-5 (CFW 6.61). Собираем **PRX**, не PBP.

## 1. Сборка (на ПК, тот же pspdev, что и для релиза)
```
make clean
make BUILD_PRX=1 DEBUG=1
```
- `BUILD_PRX=1` — по гайду («add BUILD_PRX=1 to your Makefile»), даёт `ympsp.prx` + `ympsp.elf`.
- `DEBUG=1` — `-O0 -g3` (гайд: «-g или -g3 и без -O»), иначе psp-addr2line/psp-gdb не видят строк.
- Определяется `YM_PSPLINK_DEBUG`: cwd = `ms0:/PSP/GAME/yandex_music_psp` (токен, кэш, шрифты, ассеты — на карте),
  куча 12 МБ (часть памяти занимает PSPLINK).
- После отладки вернуть релиз: `make clean && make` (EBOOT.PBP в `release/`).
- Готовые файлы последней сборки: папка `debug\` (`ympsp.prx`, `ympsp.elf`).

## 2. PSPLINK на консоли
Уже стоит: `F:\psp\GAME\psplink` (psplink.zip из https://github.com/pspdev/psplinkusb/releases/download/latest/psplink.zip).
Запуск — из меню «Игры» на XMB, консоль ждёт ПК по USB.

## 3. ПК (Windows)
USB-драйвер для PSPLINK — по гайду https://pspdev.github.io/psplink/ (страница в момент настройки не открылась,
если драйвер не стоит — обычно ставят libusb/WinUSB через Zadig на устройство PSPLINK при запущенном psplink).

## 4. Три терминала в папке с `ympsp.prx` и `ympsp.elf`
1) `usbhostfs_pc` — ждать «connected to device.», не закрывать.
2) `pspsh`, затем `debug ympsp.prx` (должно появиться «Loaded host0:/ympsp.prx»). Без отладчика: `./ympsp.prx`.
   Если GDB-сервер глючит — `reset`.
3) `psp-gdb ympsp.elf -q`, затем `target remote :10001` (покажет `_start`).
   Команды: `b`, `c`, `s`, `n`, `f`, `bt`, `p $var`, `i r`, `d`, `q`.

## 5. Адрес падения
PSPLINK при краше печатает лог с адресом. В папке с ELF:
```
psp-addr2line -e ympsp.elf <адрес>
```
(не `.prx`). Для PRX адрес — смещение внутри модуля (psplink показывает его рядом с именем модуля).
Пусто — пересобрать с `DEBUG=1`.

## 6. Полезное в pspsh
`scrshot имя.bmp`, `exit`, `poweroff`, `reset` (между сборками), `help`.
