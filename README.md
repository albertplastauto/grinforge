# GrinForge

Открытый GPU-майнер **GRIN (Cuckatoo32)** для Windows x64 / NVIDIA Ada (sm_89).
**Без developer fee** — 0 % навсегда.

Целевая машина: RTX 4060 Ti 8 GB, драйвер 617.14 (CUDA UMD 13.4), Windows 11.

---

## Зачем этот проект

Замеры на целевой карте 8 октября 2026 показали, что готовые майнеры на ней
фактически не работают:

| Майнер | Результат на RTX 4060 Ti 8 GB |
|---|---|
| GMiner 3.44 | выбрал «11GB Solver», **0.07 GPS**, 7759/8188 MiB VRAM, 0 шар за 4.5 мин, 52 Вт, dev fee **5 %** |
| lolMiner 1.98a | **не запускается**: CUDA-устройство «Unsupported device or driver version», OpenCL «Invalid buffer size» |
| «Софт от пула» (`Setup (2).zip`) | побайтово тот же GMiner 3.44 (`SHA256 9EC744DD…`); для GRIN вызывает lolMiner, который на этой карте не стартует |

Причина в том, что Cuckatoo32-солверы «mean» требуют 20–33 ГБ VRAM
(официальный `grin-miner.toml`: «the C32 reference miner requires 20GB of memory»,
абсолютный минимум по документации — 24.8 ГБ). На 8 ГБ помещается только
**lean**-солвер (~1 ГБ).

## Что внутри

| Компонент | Путь | Состояние |
|---|---|---|
| CUDA lean-солвер C32 (порт `lean.cu` Tromp) | `src/solver/` | собирается, требует toolkit |
| Независимый верификатор доказательств | `src/solver/grin_verify.hpp` | проверен |
| Стратум-клиент GRIN | `src/stratum/` | спецификация проверена на живом пуле |
| Телеметрия (NVML) и управление картой (NVAPI) | `src/monitor/` | в работе |
| Host-цикл, watchdog, failover, дашборд | `src/host/main.cpp` | написан |
| Спецификация протокола стратума | `docs/stratum-protocol.md` | проверена эмпирически |

## Сборка

Требуется: Visual Studio 2026 с workload «Desktop development with C++»,
CUDA Toolkit 13.x, CMake ≥ 3.24 (идёт в поставке VS).

```bat
cmake -S . -B build\cmake -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CUDA_COMPILER="C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4\bin\nvcc.exe"
cmake --build build\cmake
```

Артефакты: `grinforge.exe` (майнер), `solver_bench.exe` (замер GPS),
`selftest.exe` (проверка криптоядра), `stratum_client_test.exe` (проверка протокола).

## Проверка корректности

Криптоядро проверено **двумя независимыми реализациями**, потому что ошибка в
поворотах siphash или в порядке байт nonce даёт майнер, который работает и не
находит ни одной шары:

```bat
build\selftest.exe                                     && rem BLAKE2b KAT, раскладка заголовка
python tools\check_keys.py <476 hex pre_pow> <nonce>   && rem независимый BLAKE2b (hashlib)
python tools\check_siphash.py --selftest               && rem независимый siphash на Python
```

Все три совпадают с реализацией на C++/CUDA.

## Быстрый старт

```bat
grinforge.exe --pool grin.2miners.com:3030 ^
              --user <ваш_grin_адрес>.RIG1 ^
              --user-pass x ^
              --power-limit 120 --temp-limit 80
```

Сухой прогон без пула (замер GPS):

```bat
solver_bench.exe --pre-pow-file build\job.txt --seconds 60
```

## Лицензия

Наш код — MIT (`LICENSE`). Файлы, производные от `tromp/cuckoo`
(`src/solver/lean_solver.cu`, `src/solver/grin_params.hpp`,
`src/solver/grin_verify.hpp`), остаются под **The FAIR MINING License**.
Подробности и почему это не мешает нулевому dev fee — в `docs/third-party.md`.

## Ограничения и риски

* `EDGEBITS=32` для CUDA-пути у Tromp **никогда не собирался**: в Makefile есть
  `lcuda19/29/30/31`, но нет `lcuda32`. Причина найдена — сдвиг 32-битного слова
  на 32 при `PART_BITS=0`. Исправлено (`FIX-2`/`CH-1`).
* Lean-солвер по определению медленнее mean: он «memory latency bound», тогда как
  mean — «bandwidth bound». Это физическая цена того, что солвер влезает в 8 ГБ.
* Сеть GRIN (~3.5–4 kGps) добывается ASIC-фермами. Одна mid-range карта даёт
  доли процента сети, экономика майнинга отрицательная при любом тарифе выше
  ~$0.05/кВт·ч. Проект имеет смысл как инженерный/учебный и как заявка на
  bounty Tromp ($10 000 за открытый C32-солвер на 1 gps при ≤100·x Вт).
