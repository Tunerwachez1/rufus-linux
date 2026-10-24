# Rufus Linux port (этап 0)

Прототип кроссплатформенной версии Rufus: платформонезависимое ядро + HAL +
CLI. Оригинальная Windows/MinGW-сборка в корне репозитория не затрагивается.

## Лицензия

Linux-порт распространяется по **GNU General Public License v3.0 (GPLv3)**,
как и исходный проект Rufus. Полный текст лицензии:
[LICENSE.txt](../LICENSE.txt). Лицензии и уведомления об авторских правах
сторонних компонентов сохраняются.

## Решения по проекту (утверждены владельцем)

- **Автообновление выключено** — проверок новых версий в порте нет; версию
  проверяет пакетный менеджер. (Fido-скачивание ISO — отдельная функция,
  остаётся в плане этапа 4.)
- **Привилегии** — простая схема MVP: запись на блочные устройства требует
  root (`sudo`), polkit-хелпер отложен на этап 3. Запись в обычный файл
  разрешена без root через `--force` (нужно для тестов).

## Сборка

```sh
cd linux-port
meson setup build
ninja -C build
```

Зависимости: meson, ninja, gcc, libudev (пакет systemd-libs/systemd-devel).

## Использование

```sh
build/rufus-cli list            # все диски (loop/ram/zram скрыты)
build/rufus-cli list --usb      # только USB
sudo build/rufus-cli write image.iso /dev/sdX          # raw/dd
sudo build/rufus-cli write image.img.gz /dev/sdX       # сжатый образ через bled
sudo build/rufus-cli write image.iso /dev/sdX --dry-run # только проверки
sudo build/rufus-cli mktable /dev/sdX --gpt            # GPT + раздел EFI System
sudo build/rufus-cli mktable /dev/sdX --mbr            # MBR + активный FAT32(LBA)
```

`mktable` пишет таблицу разделов собственным кодом (`core/partition.c`):
GPT — по спецификации UEFI (защитный MBR, основной и резервный заголовки,
128 записей, CRC32), MBR — с сохранением уже установленного boot-кода.
Таблицы проверены независимым парсером в юнит-тестах, утилитой sfdisk
и распознаются ядром на loop-устройствах.

Форматы образов: `.iso/.img` и сжатые `.zip .gz .xz .zst .bz2 .lzma .vtsi .Z`.

### Файлы больше 4 ГиБ (FAT32)

FAT32 не может хранить файл длиннее 4 ГиБ − 1 байт, а современные
Windows-образы содержат `sources/install.wim` по 5–7 ГиБ. Конвейер
`write --copy` обходит это как настоящий Rufus:

1. **До разметки диска** дерево ISO проходится по метаданным
   (`rfd_iso_scan_bigfiles`, миллисекунды): файлы-нарушители печатаются,
   общий объём дерева становится честным 100% прогресса.
2. Любой не-WIM файл сверх лимита → **отказ до записи** с внятным
   сообщением; WIM-файлы помечаются «will be split».
3. Во время копирования oversized `*.wim` извлекается во временную папку
   `/var/tmp` и режется wimlib'ом на `install.swm`, `install2.swm`, … по
   3500 МиБ (`RFD_WIM_SPLIT_CHUNK`) — Windows setup собирает такие части
   нативно.
4. Каждая `fwrite` проверяется, размер результата сверяется с исходником:
   усечение (какое бы ни было) останавливает запись с ошибкой, а не
   оставляет битый образ.

Проверено на Win11_25H2_Russian_x64_v2.iso: предскан находит единственный
`sources/install.wim` (6,78 ГиБ) за ~10 мс.

Защитные проверки перед записью: root, блочное устройство (или `--force`),
смонтированные ФС на целевом диске/разделах, интерактивное подтверждение
(ввод имени устройства; `--yes` для скриптов), Ctrl+C → честный статус
«PARTIAL state» с размонтированием `/tmp/rufus-mnt.*` и удалением
scratch-файлов `/var/tmp/*.split-*`.

Прогресс во время обработки oversized-WIM не замирает: извлечение в
`/var/tmp` и запись `.swm`-частей делят вклад файла пополам (каждая фаза
даёт половину его байт), переходы между фазами печатаются в лог; ^C
прерывает также сам сплит wimlib'а.

Прогресс без «вздохов»: данные принудительно сбрасываются на носитель
каждые 16 МиБ (`fdatasync`, HAL-метод `sync_data`) — страничный кэш не
копит гигабайты, паузы на сброс ограничены ~1 секундой и помечаются
бейджем `[sync]` в строке прогресса. Скорость показывается по скользящему
окну 5 с (реальная скорость носителя, а не завышенная скорость записи в
кэш). Ошибка сброса (например, отошедший USB) останавливает запись с
явным сообщением.

## Структура

```
linux-port/
├── core/            платформонезависимое ядро:
│   ├── log.*        логгер
│   ├── msys_posix.c POSIX-бэкенд контракта src/ms-sys/inc/file.h
│   ├── msys_wrap.h  обёртка fd → FAKE_FD для вызовов ms-sys
│   ├── partition.*  разметка MBR/GPT собственным кодом
│   └── isocopy.*    копирование дерева ISO + сплит *.wim >4 ГиБ
├── hal/hal.h        интерфейс HAL (vtable) - ядро знает только его
├── hal-linux/       реализация HAL: udev-перечисление + block I/O
├── cli/main.c       rufus-cli: list / write / mktable
├── vendor/
│   ├── cdio-linux/  config.h + заглушки для vendored libcdio
│   └── wimlib*/     upstream wimlib 1.14.4 (src+include, без Win32/
│                    ntfs-3g модулей) и наш config.h для glibc
└── tests/           юнит-тесты (meson test) + loopback-проверка под sudo
```

## libcdio (ISO9660/Joliet/RockRidge + UDF)

Все 20 исходников собираются **без единой правки**: наш
`vendor/cdio-linux/config.h` включает POSIX/HAVE_* флаги и ставится первым
на пути включения; `vendor/cdio-linux/cdio_stubs.c` повторяет заглушки
Rufus (`cdio_open` → NULL, чтобы udf_open работал через файловый поток).

Тонкости, проверенные на живых образах:

- Открытие: `iso9660_open_ext(path, ISO_EXTENSION_ALL)` — маска обязательна,
  иначе имена остаются заглавными ISO-именами с «;1» вместо Rock Ridge.
- У Microsoft-образов ISO9660-слой содержит только README + El Torito;
  настоящие файлы лежат в **UDF**-слое → читаем через `udf_open` /
  `udf_get_root` / `udf_fopen` / `udf_read_block` (как iso.c). Важно:
  `udf_fopen` звать от «чистого» корня до обхода readdir.
- Извлечение в ISO9660: `stat->lsn` + `iso9660_iso_seek_read`.

Проверено: CachyOS (метка COS_202606, RR-имена, файлы до 7,7 МиГб
побайтово против bsdtar) и Windows 11 25H2 RU (UDF: полный листинг,
bootmgr 473 364 байта извлечён).

```sh
./build/test-cdio <image.iso> <out.bin> [/path/in/iso]   # извлечь руками
```

## Патчи к vendored-коду

Минимальные POSIX-ветки, семантика Windows-сборки сохранена:

- `src/bled/libbb.h` — вместо `#error Only Windows…` добавлена ветка
  `BLED_POSIX`: маппинг CRT (`_read/_write/_close/_dup2/_lseeki64/_pipe/
  _openU/_open_osfhandle/_unlink/_chmod/_sopen_s/_snprintf_s/_alloca`,
  флаги `_O_*`, `_SH_DENYNO`), `bb_aligned_malloc` вместо `_mm_malloc`
  (намеренно без хайджека имён - их занимает gcc-шный `mm_malloc.h`),
  рекурсивный mkdir вместо SHCreateDirectoryExU, `wait_any_nohang`,
  гвард для MinGW-специфики (`_S_IF*`, заглушки link/symlink/chown/mknod,
  fnmatch/wait, localtime_r).
- `src/bled/bled.h` — `HANDLE`/`DWORD` типы для POSIX.
- `src/bled/bb_archive.h`, `decompress_*.c` — `IF_DESKTOP(long long) int` →
  `IF_DESKTOP(int64_t)` (на glibc `long` != `long long`).
- `src/bled/zstd_deps.h` — `<crtdefs.h>` только под Windows.

## Тестирование

```sh
./build/rufus-cli list                       # без root
sudo tests/loop-test.sh                      # полная проверка на loop-устройствах:
                                             # raw-запись+верификация, gzip-запись,
                                             # отказ на смонтированном, dry-run
```

## Статус этапа 1 (в работе)

Сделано:

- **ms-sys перенесён**: вендорные `br/fat12/fat16/fat32/ntfs/partition_info`
  компилируются нетронутыми; непереносимый `src/ms-sys/file.c` заменён
  нашим `core/msys_posix.c` (pread/pwrite, тот же контракт file.h).
  Доступны все писатели MBR/PBR (win7/vista/grub2/syslinux/reactos…),
  FAT12/16/32 boot-секторы, NTFS, partition_info.
- **Разметка MBR/GPT** собственным кодом (`core/partition.c`) + CLI-команда
  `mktable`. Верификация: юнит-тесты с независимым парсером (CRC32,
  зеркалирование GPT), sfdisk, распознавание ядром на loop (`loop-test.sh`).
- **Job engine — каркас** (`core/image.*`, `core/job.*`): проба типа образа
  по PVD (ISO9660 vs raw, гибридность по сигнатуре), планировщик конвейера
  шагов с валидацией «все шаги реализуемы до первой записи». DD-план
  полностью рабочий; файловый конвейер Windows-стиля (разметка → ФС →
  копирование файлов → загрузочный код) показывается по `--copy`.
- **Большие WIM и честная запись** (`core/isocopy.c`): предскан дерева ISO
  до разметки диска; авто-сплит `*.wim` >4 ГиБ в `.swm` через vendored
  upstream wimlib; каждая fwrite проверяется, итоговый размер сверяется —
  усечение невозможно молча. Юнит-тест test-wimsplit: capture → split →
  reference → extract → побайтовая сверка.
- Юнит-тесты в CI-стиле: `meson test -C build`
  (test-msys, test-partition, test-job, test-wimsplit).

Отложено на следующие шаги этапа 1: создание ФС сверх mkfs.vfat
(mkfs-обёртки + libext2fs), syslinux/grub-установка, hash.c,
wue/wimlib-применение (сам wimlib уже вендорен ради сплита).

Критерий этапа 0 выполнен: `rufus-cli write dd <image> <device>` пишет образ
(подтверждено живой записью CachyOS ISO на USB и побайтовой сверкой).
