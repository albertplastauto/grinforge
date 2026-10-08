# Лицензии и происхождение кода

Проект распространяется под **MIT** (`LICENSE`), за исключением файлов, явно
помеченных ниже. Это разделение сделано осознанно: MIT выбран как основная
лицензия проекта (доверие, совместимость, требование заказчика), но код солвера
происходит из `tromp/cuckoo`, который распространяется под **The FAIR MINING
License**, а она не MIT.

## 1. The FAIR MINING License (John Tromp)

Файлы и их производные:

| Файл | Что изменено |
|---|---|
| `src/solver/lean_solver.cu` | порт `lean.cu`: исправление `EDGEBITS=32`, MSVC-совместимость, GRIN-заголовок, отказ от `exit()` |
| `src/solver/grin_params.hpp` | производное от `cuckatoo.h` + `siphash.hpp` |
| `src/solver/grin_verify.hpp` | производное от `cuckatoo.h` (`verify`, `setheader`) |
| `third_party/tromp-cuckoo/**` | не изменялся, используется как есть |

Полный текст: `third_party/tromp-cuckoo/LICENSE.txt`.

### Ключевое условие лицензии

> **FAIR MINING**
> Any derived miner that charges a developer fee for mining a fair coin
> — one with no premine or other form of developer compensation —
> shall offer to share half the fee revenue with the coin developers.

**GrinForge не взимает developer fee (0 %).** Условие FAIR MINING сформулировано
как обязанность, возникающая *у майнера, который взимает fee*. При нулевом fee
обязанность не активируется, и лицензия прямо разрешает:

> use, copy, modify, merge, publish, distribute, **sublicense**, and/or sell

Таким образом, распространение производного кода законно при выполнении двух
условий, которые мы соблюдаем:

1. сохранён copyright notice и текст FAIR MINING (этот файл + заголовки в файлах);
2. dev fee равен нулю — и не может быть добавлен без перехода на FAIR MINING-условия.

Требование лицензии «The above copyright notice, FAIR MINING condition, and this
permission notice shall be included in all copies or substantial portions of the
Software» выполняется: `third_party/tromp-cuckoo/LICENSE.txt` распространяется
вместе с исходниками, а каждый производный файл содержит ссылку на происхождение.

**Важно:** если кто-либо в будущем добавит dev fee, он обязан выполнить условие
FAIR MINING и делиться половиной дохода с разработчиками GRIN. Поэтому в проекте
fee не просто «не включён» — он архитектурно не предусмотрен.

## 2. BLAKE2b

`third_party/tromp-cuckoo/src/crypto/blake2b-ref.c`, `blake2.h`, `blake2-impl.h` —
reference-реализация BLAKE2, Copyright 2012 Samuel Neves.
Лицензия: **CC0 1.0 / OpenSSL / Apache-2.0 на выбор** (см. шапку `blake2.h`).
Совместима с MIT-проектом.

## 3. portable_endian.h

Public domain (Mathias Panzenböck). Мы его **не используем** — endian-хелперы
заменены прямым `memcpy` в `grin_params.hpp` (`FIX-1`), так как целевая
платформа little-endian, а заголовок требует `sys/param.h` и `htonll`, которых
нет в MSVC.

## 4. Исследовательские материалы (не входят в сборку)

| Что | Откуда | Лицензия | Как использовано |
|---|---|---|---|
| `third_party/hires-cuckatoo/` | `client8568/High-Resource-Cuckatoo-Miner` | MIT | **только как справочник** для реверса stratum-протокола 2Miners |
| `tools/stratum_probe.py` | наш код | MIT | живая проверка протокола |

Из `High-Resource-Cuckatoo-Miner` **не скопировано ни строки исполняемого кода**.
Из него установлены только факты протокола (238-байтовый `pre_pow`, 8-байтовый
big-endian nonce, форматы `login`/`getjobtemplate`/`submit`), которые
подтверждены независимым живым подключением к пулу. Факты и алгоритмы
лицензией не охраняются; охраняется конкретное выражение кода — оно не заимствовано.

Из `High-Resource-Cuckatoo-Miner` также взята идея таблицы VRAM для Cuckatoo32,
использованная только в аналитике.

## 5. Что не используется и почему

| Проект | Лицензия | Причина отказа |
|---|---|---|
| `mozkomor/GrinGoldMiner` | **GPL-3.0** | производная обязывала бы лицензировать весь проект под GPL-3.0 и отдавать исходники; кроме того, проект помечен DISCONTINUED и остановлен в январе 2020 — *до* HardFork4 |
| `mimblewimble/grin-miner` | Apache-2.0 | лицензионно совместим, но под Windows не собирается; для C32 в нём есть только mean-плагин на 20 ГБ |
| lolMiner, GMiner, Bminer | закрытые | лицензии прямо запрещают модификацию, декомпиляцию и изменение dev fee |
| `3k3r1l4rz/m1_grin_miner_fastest` | **нет LICENSE** | по умолчанию all rights reserved |

## 6. Итоговая структура лицензий

```
GrinForge (в целом)                       MIT
├── src/host/, src/stratum/, src/monitor/  MIT
├── src/solver/lean_solver.cu              FAIR MINING (производное от lean.cu)
├── src/solver/grin_params.hpp             FAIR MINING (производное от cuckatoo.h)
├── src/solver/grin_verify.hpp             FAIR MINING (производное от cuckatoo.h)
├── third_party/tromp-cuckoo/              FAIR MINING / GPL-2.0+ (как есть)
└── third_party/tromp-cuckoo/src/crypto/blake2*  CC0 / OpenSSL / Apache-2.0
```

Распространение бинарника требует приложить текст FAIR MINING и copyright
John Tromp — см. `third_party/tromp-cuckoo/LICENSE.txt`.
