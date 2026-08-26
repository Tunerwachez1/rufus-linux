# План портирования Rufus 4.15 на Linux

> Задача: переписать Rufus так, чтобы он работал на всех дистрибутивах Linux.
> Базовая версия: Rufus 4.15 (~37 тыс. строк собственного кода + ~153 тыс. строк
> vendored-библиотек), чистый Win32 API, C11, лицензия GPLv3.
>
> **Статус: этап 0 выполнен, этап 1 в работе** (ms-sys, разметка MBR/GPT,
> libcdio, конвейер записи и сплит >4 ГиБ WIM готовы — см. раздел 7 и
> `linux-port/README.md`).

## Решения владельца проекта (зафиксированы)

1. **Автообновление выключено.** Проверка новых версий Rufus (и dbx-апдейты
   как её часть) в порте отсутствует — версию проверяет пакетный менеджер.
   Fido-скачивание ISO — отдельная функция, остаётся в плане.
2. **Привилегии — простая схема:** запуск от root (`sudo`), polkit-хелпер
   отложен. Запись на блочное устройство требует root; запись в обычный файл
   разрешена без root через `--force`.
3. **GUI — минимально достаточный** (уточнение 2026-08): не копировать все
   диалоги оригинала; главное окно «образ + устройство + запись + прогресс»,
   адекватный внешний вид без полировки.
4. **Распространение — портативная папка с .sh-лаунчером** (уточнение
   2026-08): как портативные программы под Windows — рядом с бинарём лежит
   `rufus.sh`, пользователь его открывает и сразу появляется GUI. Без
   установщиков и самоизвлекающихся архивов. Состав поставки: `rufus.sh`
   (`cd "$(dirname "$0")"` + запуск через bundled ld-linux с --library-path),
   `rufus-gui`, `rufus-cli`, `lib/` (несистемные библиотеки). Раздаётся одним
   tar.gz/zip. Нативные пакеты deb/rpm/AUR — опционально позже, не условие
   релиза.

---

## 1. Что выяснено при изучении проекта

Rufus — монолитное Win32-приложение **без какого-либо слоя абстракции над ОС**:
`windows.h` подключён в 44 файлах `src/`, Windows API вызывается напрямую почти
везде. Сборка под Linux не предусмотрена вовсе (autotools используется только
для кросс-компиляции MinGW).

### Ключевые подсистемы и их привязка к Windows

| Подсистема | Файлы | Чем сделано на Windows | Переносимость |
|---|---|---|---|
| UI | ui.c, stdlg.c, darkmode.c, icon.c (~5.5k строк) | голый Win32 (DialogBox, CreateWindowEx), тёмная тема через недокументированные ordinals UxTheme | ❌ переписывать целиком |
| Перечисление USB-устройств | dev.c | SetupDi*, CM_*, IOCTL_USB_GET_NODE_CONNECTION_*, IOCTL_USB_HUB_CYCLE_PORT | ❌ заменить на udev/sysfs |
| Дисковая логика | drive.c (2.7k), process.c (1k) | `\\.\PhysicalDriveN`, DeviceIoControl, VDS/COM, NtQuerySystemInformation | ❌ заменить на POSIX/ioctls |
| Форматирование | format.c (2k) | FormatEx из fmifs.dll, IVdsVolumeMF3_FormatEx2 | ❌ заменить на mkfs.* |
| Запись MBR/PBR | ms-sys (vendored, 3.6k) | чистый переносимый C, байтовые шаблоны загрузчиков (mbr_win7, grub2, syslinux, reactos…) | ✅ как есть |
| FAT12/16/32 «Large FAT32» | format_fat32.c | собственная сборка boot-секторов | ✅ как есть |
| ext2/3/4 | format_ext.c + vendored e2fsprogs/libext2fs (31.8k) | переносимый C, кроме `ext2fs/nt_io.c` | ✅ заменить только I/O-шлюз |
| Парсинг ISO 9660/Joliet/Rock Ridge/UDF | iso.c + vendored libcdio (17.7k) | libcdio, физический доступ уже отключён заглушками | ✅ почти как есть |
| WIM (Windows To Go, unattend.xml) | wue.c, vhd.c + vendored wimlib (62.9k) | wimlib патчен под Rufus (UTF‑16 `U`-функции); сам wimlib изначально кроссплатформенный | ⚠️ взять upstream wimlib |
| Распаковка образов (.gz/.xz/.zstd/.zip/.vtsi) | bled (33k, busybox-derived) | почти переносим | ✅ минимальные правки |
| Syslinux | syslinux.c + libinstaller/libfat | один файл win/ntfssect.c непереносим | ✅ остальное переносимо |
| FreeDOS | dos.c + ресурсы res/freedos | ресурсы переносятся, код почти чистый | ✅ |
| Хэши MD5/SHA1/256/512 | hash.c | собственная реализация с SHA-NI | ✅ как есть |
| Проверка bad blocks | badblocks.c | порт из e2fsprogs под Win32 I/O | ✅ вернуть POSIX I/O |
| SMART | smart.c | IOCTL_SCSI_PASS_THROUGH_DIRECT | ⚠️ на Linux это SG_IO ioctl — близкий аналог, умеренная переделка |
| Локализация | localization.c, parser.c, res/loc (37 языков, формат .loc) | собственный парсер, чистый C | ✅ как есть |
| XML/regex/unattend | xml.c, parser.c, cregex_* | чистый C | ✅ как есть |
| Сеть (обновления, Fido — скачивание ISO Microsoft) | net.c | wininet | ❌ → libcurl |
| Подписи/Authenticode | pki.c | WinVerifyTrustEx + собственный RSA | ⚠️ RSA-логика остаётся, Authenticode заменяется |
| VHD/VHDX/FFU | vhd.c | virtdisk.dll | ❌ свой парсер VHDX или qemu-img; FFU отбросить |
| Настройки | stdfn.c | реестр | ❌ → INI в ~/.config |

**Итог:** полностью переписать нужно ~14 тыс. строк (UI + устройства +
форматирование + сеть). Ещё ~60 тыс. строк vendored-кода и ~10 тыс.
собственного переезжают с минимальными правками.

---

## 2. Стратегия

Рассматривались три варианта:

1. **Запуск оригинала через Wine** — отвергнуто: хрупко, требует прав root
   внутри префикса Wine, не даёт нативной интеграции, не «работа на дистрибутивах».
2. **Форк существующих Linux-аналогов** (Ventoy, balenaEtcher, WoeUSB,
   Popsicle, Impression) — отвергнуто как основную стратегию: ни один не
   покрывает функциональность Rufus (Windows-загрузчики, WIM, Fido, dbx).
3. **Рекомендуемое: рефакторинг Rufus в кроссплатформенное приложение**
   с разделением на платформонезависимое ядро и HAL-слой ОС, новый GTK4-GUI
   и headless CLI. Сохраняем максимум проверенного кода (ms-sys, bled,
   libext2fs, libcdio, wimlib, hash, локализация) вместо написания с нуля.

Язык и тулкит: **C + GTK4** (ближайший к исходному стеку, GTK4 есть везде;
альтернатива — Qt6/C++, если важнее единый стиль KDE — но тогда придётся
переписывать и переносимые C-части или оборачивать их FFI). CLI-режим
обязателен: он же станет тестовым фундаментом до появления GUI.

---

## 3. Целевая архитектура

```
┌────────────────────────────────────────────────────────┐
│  Презентация                                            │
│   • rufus-gtk  — GUI на GTK4 (главное окно, диалоги)    │
│   • rufus-cli  — headless-интерфейс (скрипты, CI, тесты)│
├────────────────────────────────────────────────────────┤
│  Ядро (платформонезависимый C11) — librufus-core        │
│   job engine (конвейер задач записи), определение типа  │
│   образа, iso.c / wue.c / syslinux.c / dos.c /          │
│   format_fat32.c / hash.c / xml.c / parser.c /          │
│   localization.c / pki.c(RSA) / cregex                  │
├────────────────────────────────────────────┬───────────┤
│  Vendored (как есть): ms-sys, bled,        │  HAL      │
│  libcdio, libext2fs, wimlib(upstream),     │  (vtable  │
│  syslinux libfat+libinstaller              │  функций) │
├────────────────────────────────────────────┴───────────┤
│  Реализации HAL                                         │
│   hal-linux: udev, /dev/sdX, SG_IO, BLKRRPART,          │
│              mkfs.*, udisks2/polkit                     │
│   hal-win32 (опционально, для сохранения паритета)      │
└────────────────────────────────────────────────────────┘
```

Ключевое решение: **HAL в виде структуры функций** (`enum devices`,
`open_target`, `lock`, `unlock`, `write_sectors`, `read_sectors`,
`partition_mbr_gpt`, `format_fs`, `mount`, `unmount`, `eject`,
`cycle_port`, `query_smart`). Весь код ядра общается только с HAL — это же
даёт возможность юнит-тестирования ядра на loopback-устройствах без железа.

---

## 4. Замена подсистем: Windows API → Linux

| Что | На Windows сейчас | На Linux делаем |
|---|---|---|
| Поиск устройств | SetupDiGetClassDevs(GUID_DEVINTERFACE_DISK) + CM_* | libudev: фильтр `block`/`disk`, атрибуты ID_VENDOR/ID_MODEL/serial, removable, размер (`size`), родительский USB-узел, номер порта |
| Открытие цели | CreateFile(`\\.\PhysicalDriveN`) | `open("/dev/sdX", O_RDWR \| O_EXCL \| O_DIRECT)`; EBUSY = занято |
| Геометрия | IOCTL_DISK_GET_DRIVE_GEOMETRY_EX | ioctl BLKSSZGET / BLKGETSIZE64 |
| Блокировка/размонтирование томов | FSCTL_DISMOUNT_VOLUME | размонтирование через udisks2 D-Bus (fallback: umount от хелпера); сканирование /proc/*/fd для поиска процессов-держателей (замена process.c) |
| Разметка MBR/GPT | IOCTL_DISK_CREATE_DISK + layout-ex | запись структур напрямую байтами (MBR уже умеет ms-sys; GPT — ~200 строк собственного кода: заголовок, записи, backup, CRC32), затем ioctl BLKRRPART / partx -u |
| Создание ФС | FormatEx(fmifs.dll), VDS | вызов mkfs.vfat/mkfs.exfat/mkfs.udf/mkntfs (dosfstools/exfatprogs/udftools/ntfs-3g — объявленные soft-dependencies); ext2/3/4 — встроенный vendored libext2fs; Large-FAT32 — собственный код format_fat32.c |
| Монтирование | автоматическое | udisks2 D-Bus Mount(); fallback — mount(8) через polkit-хелпер |
| Безопасное извлечение | CM_Request_Device_Eject | udisks2 PowerOff(); fallback — ioctl USBDEVFS_DISCONNECT |
| Сброс USB-порта | IOCTL_USB_HUB_CYCLE_PORT | usbutils/USBDEVFS_RESET (частичная замена; функция второстепенная) |
| SMART/hdd-vs-flash | IOCTL_SCSI_PASS_THROUGH_DIRECT | ioctl SG_IO (та же семантика SCSI PASS THROUGH) |
| Bad blocks | порт e2fsprogs на Win32 I/O | тот же алгоритм поверх read/write с O_DIRECT |
| Сеть | wininet | libcurl (+ системный TLS) |
| Загрузчики ldlinux | ресурсы exe + SHA256 из db.h + скачивание | файлы данных пакета (/usr/share/rufus/) + та же проверка SHA256 |
| Проверка подписи | WinVerifyTrust/Authenticode | своя RSA-проверка (уже есть в pki.c) + GPG-подписи релизов |
| Настройки | реестр | INI/GKeyFile в $XDG_CONFIG_HOME/rufus |
| Тёмная тема | ordinals UxTheme + subclassing | тема GTK4/libadwaita автоматически |
| Уведомления | MessageBox/tray | libnotify + диалоги GTK |
| Локализация | embedded .loc (37 языков) | тот же формат .loc тем же парсером; опционально конвертер в gettext |

---

## 5. Судьба функций Rufus на Linux

| Функция | Статус | Комментарий |
|---|---|---|
| DD-запись образов (включая сжатые gz/xz/zstd/zip/vtsi) | ✅ прямая | bled переносим |
| Запись ISO файловым копированием (Linux ISO) | ✅ | libcdio извлекает, разметка+ФС через HAL |
| Запись Windows-ISO файловым копированием | ✅ | + установка MBR/PBR из ms-sys |
| UEFI:NTFS (гибридный бутсектор) | ✅ | логика маскировки PBR переносима |
| Syslinux/ISOLINUX | ✅ | libinstaller + скачивание версий |
| GRUB2 (MBR + EFI) | ✅ | копирование EFI-файлов + core.img, как сейчас |
| FreeDOS / MS-DOS | ✅ | ресурсы переносятся |
| Large FAT32 (>32 ГБ) | ✅ | собственный код |
| ext2/3/4 | ✅ | встроенный libext2fs |
| NTFS / exFAT / UDF | ⚠️ soft-deps | внешние mkfs.ntfs/mkfs.exfat/mkfs.udf; при отсутствии — деградация с понятным сообщением (или позже — свои реализации) |
| Persistent partition (casper-rw) | ✅ | дополнительный раздел + label |
| Хэш-суммы MD5..SHA512 | ✅ | свой код |
| Bad blocks check | ✅ | свой код |
| Обновление Secure Boot dbx | ✅ | res/dbx + чистый код |
| Wue: unattend.xml, обход TPM/SecureBoot/RAM, OOBE | ✅ | модификация install.wim/boot.wim через upstream wimlib работает нативно на Linux |
| Windows To Go | ⚠️ сложно | wimlib apply — ок; ESP создаётся сами; **нет bcdboot** → генерировать BCD-hive самостоятельно (hivex или собственный writer реестра). Отдельный этап |
| Fido: скачивание официальных MS ISO | ✅ | распарсить LZMA-PowerShell-скрипт cregex'ом + libcurl (логика уже есть в net.c) |
| VHD/VHDX чтение | ⚠️ позже | свой парсер VHDX (спецификация открыта) или qemu-img+nbd |
| FFU создание/чтение | ❌ отбросить | глубоко проприетарный формат Microsoft |
| Автообновление Rufus | ❌ выключено | по решению владельца; версию проверяет пакетный менеджер |
| Fido-скачивание официальных MS ISO | ✅ | отдельная функция, остаётся: cregex + libcurl (логика уже есть в net.c) |

---

## 6. Модель привилегий

**Принятое решение (владелец): простая схема с запуском от root.**

1. Запись на блочные устройства выполняется из-под `sudo` — CLI проверяет
   `geteuid()` и даёт понятную ошибку.
2. Запись в обычные файлы (`--force`) разрешена без root — это нужно для
   тестов и не несёт рисков.
3. Защита от ошибок остаётся на месте даже под root: отказ при смонтированных
   ФС на целевом диске/разделах, интерактивное подтверждение вводом имени
   устройства, `--dry-run`.
4. Полноценный polkit-хелпер (GUI без root + привилегированный бэкенд через
   D-Bus) отложен на этап 3; архитектура HAL уже позволяет разделить
   «логику» и «доступ к железу», так что перенос не потребует переписывания
   ядра.
5. Flatpak: raw-доступ к /dev ограничен песочницей — основной канал
   распространения AppImage/deb/rpm; Flatpak позже, через udisks2.

---

## 7. План работ по этапам

Оценки — для одного опытного разработчика C/GTK.

### Этап 0 — Разведочный прототип ✅ ВЫПОЛНЕН
- Новая система сборки: **Meson** (`linux-port/`), Windows-сборка не затронута.
- Собрано на Linux: **bled целиком** (все 37 исходников) через минимальные
  POSIX-патчи `libbb.h`/`bled.h`/`bb_archive.h`/`zstd_deps.h`.
- HAL-vtable (`linux-port/hal/hal.h`) + реализация hal-linux: перечисление
  дисков через libudev, эксклюзивное открытие, позиционная запись/чтение,
  flush, BLKRRPART, список разделов для проверок безопасности.
- CLI: `rufus-cli list [--usb]`, `write <image> <device>` — raw/DD и сжатые
  образы (.zip/.gz/.xz/.zst/.bz2/.lzma/.vtsi/.Z) через bled; прогресс,
  скорость/ETA, отмена Ctrl+C, dry-run, защита от записи на смонтированные
  устройства, интерактивное подтверждение.
- **Критерий выполнен:** запись образа работает и верифицирована (raw/gzip/
  xz/zstd побайтовое совпадение на файлах; loopback-тест под sudo —
  `sudo linux-port/tests/loop-test.sh`). Список устройств проверен на живой
  системе (udev: USB/ATA корректно).
- Отложено на этап 1 (тянут src/rufus.h со всем Win32): ms-sys, hash.c,
  libcdio, libext2fs, wimlib.

### Этап 1 — Ядро (3–6 недель) — В РАБОТЕ
- ✅ **ms-sys отвязан от Win32** (первая задача этапа): вендорные исходники
  собираются под Linux нетронутыми; непереносимый `src/ms-sys/file.c`
  заменён POSIX-бэкендом `linux-port/core/msys_posix.c`. Доступны Windows
  MBR/PBR (win7/vista/grub2/syslinux/reactos…), FAT12/16/32 BR,
  partition_info. Проверено юнит-тестом test-msys (28 проверок).
- ✅ **Разметка MBR/GPT собственной записью** (`core/partition.c`): GPT по
  спецификации UEFI (protective MBR, primary+backup заголовки, 128 записей,
  CRC32), MBR с сохранением boot-кода. CLI: `mktable --gpt|--mbr`.
  Верификация: независимый парсер в test-partition, sfdisk, распознавание
  ядром на loop-устройствах (loop-test.sh, шаги 5–6).
- HAL-vtable: вынести все вызовы устройств ядра за интерфейс — частично
  (CLI/HAL уже разделены).
- ✅ **libcdio перенесён** (ISO9660/Joliet/RockRidge + UDF): 20 исходников
  собираются нетронутыми через Linux-config.h (`linux-port/vendor/cdio-linux/`).
  Проверено на двух живых образах: CachyOS (метка, RR-имена, извлечение
  до 7,7 МиБ побайтово против bsdtar) и Windows 11 25H2 RU (двухслойный
  ISO9660+UDF: настоящий контент читается из UDF-слоя — bootmgr 473 КБ;
  заглушки cdio_open как в оригинале). Тест test-cdio принимает любой ISO.
- ✅ **Конвейер записи (job engine) собран целиком**: проба типа образа +
  планировщик + исполнители. `write --copy` выполняет полный Windows-сценарий:
  разметка MBR → mkfs.vfat (dosfstools, soft-dep) → монтирование → рекурсивное
  копирование дерева из ISO (UDF или ISO9660 слой, прогресс/отмена/лимит)
  → Windows MBR + FAT32 PBR от ms-sys. Тест: loop-test.sh шаг 7 на живом
  Win11 ISO (bootmgr на результатной ФС сверяется байтами).
- ✅ **Файлы >4 ГиБ на FAT32 закрыты полностью**: предскан дерева ISO по
  метаданным до разметки диска; не-WIM нарушители → отказ до записи,
  `*.wim` автоматически режутся на `install.swm`/`install2.swm` (по
  3500 МиБ) через вендорный **upstream wimlib 1.14.4** — подключён раньше
  этапа 4 именно ради сплита. Каждая fwrite проверяется, итоговый размер
  файла сверяется с исходником. Мотивация: первая живая запись Win11
  молча обрезала install.wim (7,27 ГиБ → потолок FAT32) из-за
  непроверенного fwrite — дефект найден разбором флешки и закрыт.
  Тест test-wimsplit: capture → split → reference → extract → побайтовая
  сверка.
- Создание ФС (mkfs-обёртки + libext2fs).
- Syslinux/GRUB/FreeDOS/UEFI:NTFS/persistent — перенос соответствующей логики.
- Юнит-тесты на loopback-устройствах (`losetup`) в CI — частично сделано
  (`meson test` + `sudo tests/loop-test.sh`, шаги 1–6).
- **Критерий:** CLI покрывает все базовые сценарии Rufus без GUI.

### Этап 2 — GUI (упрощённо по решению владельца)
- GTK4-приложение, минимально достаточный интерфейс: выбор образа (файловый
  диалог), выбор устройства (список из HAL), выбор режима если нужен,
  большая кнопка записи; прогресс с процентами/скоростью/отменой; строка
  статуса или простой лог внизу; подтверждение стирания.
- Без попыток повторить About/License/Settings/тёмную тему оригинала —
  системная тема GTK4 сама даёт адекватный вид.
- Локализация .loc — позже, не в первом GUI.
- **Критерий:** основные сценарии CLI доступны мышью из окна.

### Этап 3 — Linux-специфика и безопасность (2–4 недели)
- polkit-хелпер + D-Bus, отказ от запуска всего приложения от root.
- Монтирование через udisks2, обработка занятых устройств (замена process.c),
  eject/power-off, SMART, badblocks, сброс кэша и синхронизация.
- Обработка разнообразия дистрибутивов: детект наличия mkfs.*, версии syslinux,
  udev-правила, SELinux/AppArmor-нюансы задокументировать.

### Этап 4 — Продвинутые функции (по желанию, 4–8 недель)
- Wue (unattend/TPM-bypass) на upstream wimlib.
- Windows To Go (генерация BCD-hive, ESP).
- Fido-скачивание MS ISO.
- Чтение VHDX.

### Этап 5 — Упаковка «портативная папка + rufus.sh» и выпуск
- **Основной канал по решению владельца:** поставка одной папкой:
  `rufus.sh` (лаунчер: cd в свою директорию, запуск бинаря через bundled
  `lib/ld-linux` c `--library-path lib`), `rufus-gui`, `rufus-cli`,
  `lib/` — нестандартные библиотеки рядом, без установки и извлечения.
  Раздача одним tar.gz/zip; открыл rufus.sh — появилось окно.
- Требование к системе: glibc + GTK4 (при отсутствии — понятное сообщение
  «установите gtk4»).
- Права: GUI стартует без root, привилегированная запись — через
  pkexec/polkit с одним запросом пароля (или запуск через sudo).
- Нативные пакеты deb/rpm/AUR — опционально позже, не условие релиза.
- CI: сборка артефакта + smoke-тест в qemu-контейнерах ubuntu/fedora/arch.
- GPG-подпись, README со скриншотом и инструкцией «распакуй, открой
  rufus.sh».

**Суммарно:** MVP (CLI + базовый GUI, DD/ISO/Syslinux/GRUB/FAT/ext) ≈
2–3 месяца; полный паритет с Windows-версией ≈ 5–6 месяцев.

---

## 8. Риски

| Риск | Митигация |
|---|---|
| Патчи Rufus в wimlib (UTF-16) конфликтуют с upstream | перейти на официальный upstream wimlib (изначально кроссплатформенный), UTF-8 пути на Linux |
| Разные версии mkfs.* на разных дистрибутивах | runtime-детект возможностей, тестовая матрица CI, понятные сообщения о недостающих пакетах |
| Гонки перемонтирования таблицы разделов (udev settle) | BLKRRPART + ожидание udev-событий перед созданием ФС |
| Windows To Go без bcdboot | отдельный этап, прототип генерации BCD-hive; если слишком дорого — фича помечается экспериментальной |
| Flatpak-ограничения на /dev | основной канал — AppImage/deb/rpm; Flatpak через udisks2 |
| Лицензии vendored-кодa | всё уже GPL-совместимо (сам Rufus GPLv3), проверить при переходе на upstream wimlib |
| Разметка GPT своими силами | объём небольшой (~200–300 строк), но обязательно покрыть тестами (gdisk/sfdisk -l верификация в CI) |

---

## 9. Первые конкретные шаги

1. Завести репозиторий-форк, ветку `linux-port`; добавить Meson-скелет,
   не ломая существующую сборку.
2. Собрать на Linux: `ms-sys`, `bled`, `hash.c`, `cregex`, `xml.c`,
   `localization.c` — они должны собираться почти сразу.
3. Написать `hal-linux/devices.c` на libudev (список дисков) и
   `hal-linux/blockio.c` (open/write/read/eject) — фундамент всего остального.
4. CLI: `list`, `write --dd`, затем `write --iso`.
5. После работоспособного CLI — принимать решения по GTK4-GUI.
