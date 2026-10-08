# GrinForge

Открытый GPU-майнер **GRIN (Cuckatoo32)** для Windows x64 / NVIDIA Ada (sm_89).
**Без developer fee** — 0 % навсегда.

Целевая машина: RTX 4060 Ti 8 GB, драйвер 617.14 (CUDA UMD 13.4), Windows 11.

---

## English summary

**GrinForge** is an open-source **GRIN (Cuckatoo32)** GPU miner for Windows x64 and
NVIDIA Ada (sm_89), with **zero developer fee**. It contains a lean Cuckatoo32 CUDA
solver, its own GRIN stratum client, pool failover, a thermal/stall watchdog, NVML
telemetry and optional GPU control.

It is **verified end to end**: the miner has found Cuckatoo32 cycles, checked each one
with an independent verifier, submitted them, and had them **accepted by 2miners** — the
pool's own public API reports our wallet's hashrate and our worker among the active
miners.

| Miner | Reported hashrate | Power | Accepted shares |
|---|---|---|---|
| GMiner 3.44 | 0.07 GPS (self-reported) | 52 W | **0**, while charging a 5% fee |
| lolMiner 1.98a | does not start | — | — |
| **GrinForge** | 0.055 GPS local / **0.07 GPS per the pool** | ~70 W | **yes** |

Be realistic about the numbers: on an 8 GB card only the *lean* solver fits (the fast
*mean* solver needs 20–33 GB), so a few hundredths of a GPS is the ceiling here. Widely
quoted figures such as "RTX 4060 Ti ≈ 0.65 H/s" are not reachable on 8 GB. GRIN is
dominated by ASICs; this project is about a correct, transparent, fee-free
implementation, not about out-earning an ASIC.

Properties worth knowing:

* **The mining process never needs administrator rights.** GPU control flags report
  refusal with a reason instead of silently doing nothing; the one privileged action is a
  separate one-shot `--install-gpu-profile`, which exits immediately.
* **Wallet guard.** `--allow-address` makes the miner refuse to start unless the address
  it would mine to is exactly the one you intend, so a tampered `.bat` cannot silently
  redirect your hashrate.
* **Measured GPU tuning** (`--tune`): hashrate turned out to be independent of every GPU
  profile tried (±1 %), including halving the core clock. The tuning is documented with
  numbers rather than advice.
* **Licence:** MIT, except the solver files derived from `tromp/cuckoo`, which remain
  under The FAIR MINING License. See [`LICENSE`](LICENSE) and
  [`docs/third-party.md`](docs/third-party.md).

Quick start:

```bat
scripts\build_all.bat
build\cmake\grinforge.exe --pool grin.2miners.com:3030 ^
    --user <your-grin-wallet>.RIG1 --allow-address <your-grin-wallet>
```

All validation was done on driver 617.14 / CUDA 13.4 / MSVC 14.51.36231; the exact
versions and the re-validation procedure are in
[`docs/validated-environment.md`](docs/validated-environment.md). The detailed technical
documentation below is in Russian.

---

## Текущий статус (8 октября 2026)

| Компонент | Состояние |
|---|---|
| Сборка MSVC 14.51 + CUDA 13.4 + sm_89 | ✅ работает, 6 исполняемых файлов |
| Заголовок GRIN → ключи siphash | ✅ проверено двумя независимыми реализациями на Python |
| device- и host-siphash | ✅ 0 расхождений из 64, плюс сверка на всех 2^20 рёбрах |
| Модель узла Cuckatoo (`sipnode>>1` + слот чётности) | ✅ подтверждена консенсус-вектором GRIN `V1_32` |
| Тримминг (правило partner-slot) | ✅ совпадает с CPU-репликацией логики ядра по каждому nonce |
| Поиск циклов | ✅ **найдены и проверены реальные решения**: 6-циклы (C20/P6) и 42-циклы (C29/P42) |
| Стратум-клиент | ✅ проверен на живом 2miners: login, job 238 байт, keepalive |
| Failover, watchdog (термогард, детект зависания) | ✅ реализованы |
| Телеметрия NVML | ✅ живые температура, кулер, мощность, частоты, VRAM |
| Управление картой | ⚠️ power limit требует прав администратора; undervolt на этой карте **невозможен** (`numBaseVoltages=0`); кулер через NVAPI драйвер блокирует |
| Скорость | ⚠️ **~0.055 GPS** локально при ~70 Вт; пул оценивает нас в **0.07 GPS** |
| **Принятая пулом шара** | ✅ **ЕСТЬ** — см. доказательства ниже |

### Доказательство работоспособности (сквозное)

```
[21:04:17.168] submitted solution height=4053661 job_id=0 nonce=0 lz=1
[21:04:17.216] recv: {"id":"49","jsonrpc":"2.0","method":"submit","result":"ok"}
[21:04:17.262] submit accepted by the pool
```

Локальный счётчик: `sol=1 sub=1 acc=1 rej=0 stale=0`.

Независимое подтверждение со стороны пула — публичный API
`https://grin.2miners.com/api/accounts/<кошелёк>`:

```
currentHashrates -> {"32":0.07}
hashrates        -> {"32":0.07}
```

и наш воркер присутствует в `https://grin.2miners.com/api/miners` среди активных
майнеров. То есть путь «нашли цикл → проверили → отправили → пул зачёл» замкнут
целиком, а не только по нашей стороне.


**Ожидаемая частота решений.** Математическое ожидание числа циклов длины
`PROOFSIZE` на граф равно **1 / PROOFSIZE** и не зависит от `EDGEBITS`: 1/6 для
6-циклов, **1/42 ≈ 2.4 %** для 42-циклов. При ~18.5 с на граф это в среднем одно
решение примерно за 13 минут, поэтому отсутствие решений на коротких прогонах
ничего не доказывает (за 24 графа вероятность не увидеть ни одного — 56 %).

Наблюдённые подтверждения: 7 решений на 24 графах при C20/P6 и 4 решения на
170 графах при C29/P42, все прошли `grin_verify`.


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

> **Проверенная среда зафиксирована.** Все результаты в этом файле получены на
> драйвере **617.14**, CUDA **13.4** (V13.4.59) и MSVC **14.51.36231**. Точные версии,
> перечень проверенного и набор команд для перепроверки — в
> [`docs/validated-environment.md`](docs/validated-environment.md). Сменился драйвер,
> CUDA или компилятор — прогоните проверку заново.

Требуется: Visual Studio 2026 с workload «Desktop development with C++»,
CUDA Toolkit 13.x, CMake ≥ 3.24 (идёт в поставке VS).

Одной командой (поправьте `VSDIR` / `CUDAROOT` внутри, если у вас другие пути):

```bat
scripts\build_all.bat
```

Вручную:

```bat
cmake -S . -B build\cmake -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CUDA_COMPILER="C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4\bin\nvcc.exe"
cmake --build build\cmake
```

Артефакты в `build\cmake\`: `grinforge.exe` (майнер), `solver_bench.exe` (замер GPS и
самопроверка), `solver_bench_tiny.exe` / `solver_bench29.exe` (окна поиска циклов),
`stratum_client_test.exe` (живая проверка протокола).

## Проверка корректности

Криптоядро проверено **двумя независимыми реализациями**, потому что ошибка в
поворотах siphash или в порядке байт nonce даёт майнер, который работает и не
находит ни одной шары:

```bat
build\cmake\solver_bench.exe --selftest                  :: BLAKE2b KAT, раскладка заголовка
build\cmake\solver_bench.exe --pre-pow-file build\job.txt --device-check
python tools\check_keys.py <476 hex pre_pow> <nonce>     :: независимый BLAKE2b (hashlib)
python tools\check_siphash.py --selftest                 :: независимый siphash на Python
```

Все три совпадают с реализацией на C++/CUDA.

## Быстрый старт

```bat
grinforge.exe --pool grin.2miners.com:3030 ^
              --user <ваш_grin_адрес>.RIG1 ^
              --pass x ^
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

## Привилегии: майнер никогда не требует прав администратора

Это осознанное свойство безопасности, а не временное ограничение.

**Что это даёт.** Процесс майнинга не может менять частоты, лимиты питания и кулер,
не может писать за пределы своей папки, и если бинарник кто-то подменит, у него не
будет повышенных прав. Для программы, которая круглосуточно работает на машине и
принимает данные из сети, это существенно: компромисс майнера не превращается в
компромисс системы.

| Действие | Нужен администратор? |
|---|---|
| Майнинг, stratum, watchdog, failover, HTTP API, статистика | **нет** |
| Чтение частот, температур, мощности, оборотов (NVML) | **нет** |
| Проверка кошелька по allowlist (`--allow-address`) | **нет** |
| Разовые ключи управления картой (`--power-limit`, `--lock-core`, `--undervolt-*`, `--fan`) | да, но они и не нужны для работы |
| Разовая настройка профиля GPU (`--install-gpu-profile`) | да, один раз на машину |

**Как это устроено, чтобы не было «тихих» отказов.** Ключи управления картой при
запуске без прав не делают вид, что сработали: каждый отказ возвращается с причиной и
флагом `needs_elevation` и печатается в лог. Разовая настройка вынесена в отдельный
режим, который применяет профиль и **сразу завершается** — привилегированный процесс
не остаётся жить на машине:

```bat
:: один раз, в консоли администратора
"build\cmake\grinforge.exe" --install-gpu-profile 2500
install-gpu-clock-task.bat        :: чтобы настройка переживала перезагрузки
```

Обычный запуск майнера — как всегда, без прав администратора.

**Майнер сам подскажет.** Он не может определить наличие ограничения частоты напрямую
(`nvidia-smi` не сообщает об этом: `Max Clocks` всегда показывает аппаратный максимум),
поэтому через ~90 секунд под нагрузкой он оценивает наблюдаемую частоту и пишет в лог
либо что профиль эффективности активен, либо точную команду для его включения.

## Ограничение частоты ядра (измеренный оптимум)

Подбор режимов (`grinforge --tune`) показал, что **хешрейт не зависит от настроек
карты**: 0.0538–0.0549 GPS при разбросе ~1 % между всеми профилями, включая снижение
частоты ядра почти вдвое. Поэтому частота покупает не скорость, а эффективность:
потолок буста 2500 МГц даёт тот же GPS при заметно меньшем потреблении.

Применено на этой машине:

```bat
:: разово, нужны права администратора
nvidia-smi --lock-gpu-clocks=0,2500

:: снять
gpu-unlock.bat            :: = nvidia-smi --reset-gpu-clocks
```

Используется **потолок** (`0,2500`), а не жёсткая прибивка (`2500,2500`): под нагрузкой
карта всё равно встаёт на ~2490 МГц, но в простое может сбрасывать частоту, не поднимая
потребление рабочего стола.

### Ограничение действует только пока работает майнер

Держать потолок 2500 МГц постоянно неправильно: игры и видео хотят полный буст до
3105 МГц, а выгода от ограничения — всего 3–7 Вт (порядка 20 рублей в месяц). Поэтому
переключение автоматизировано, а не делается руками через UAC.

`install-gpu-clock-guard.bat` создаёт задачу «GrinForge GPU clock guard», которая раз в
минуту проверяет, запущен ли `grinforge.exe`, и **трогает GPU только при смене
состояния**:

| Состояние | Что делает сторож |
|---|---|
| Майнер работает | `nvidia-smi --lock-gpu-clocks=300,2500` |
| Майнер не работает | `nvidia-smi --reset-gpu-clocks` — полный буст для игр |

Проверено на этой машине:

```
22:10:02  miner running  -> capping to 300..2500 MHz
22:11:02  miner stopped  -> resetting clocks (full boost for games/video)   (210 МГц, 7.4 Вт в простое)
22:12:02  miner running  -> capping to 300..2500 MHz
```

Сторож — это **отдельная маленькая задача планировщика, а не майнер**: сам процесс
майнинга по-прежнему работает без прав администратора (см. раздел о привилегиях).

```bat
:: снять ограничение прямо сейчас, не дожидаясь минуты
gpu-unlock.bat                     :: или nvidia-smi --reset-gpu-clocks

:: логи сторожа и удаление
type %ProgramData%\grinforge-gpu-guard.log
schtasks /Delete /TN "GrinForge GPU clock guard" /F
```

Если пользователь выставит профиль вручную (Afterburner, NVIDIA app), сторож его не
перебьёт, пока состояние майнинга не изменится — именно для этого он сравнивает
состояние с сохранённым, а не дёргает GPU каждую минуту.

Замеры и обоснование — в `docs/performance-notes.md`, раздел 7.

## Пиковые часы цен API (учёт в работе)

Проект ведётся с оглядкой на **цены DeepSeek API**, а не на тарифы на
электроэнергию. Источник — [Models & Pricing, DeepSeek API Docs](https://api-docs.deepseek.com/quick_start/pricing/):

> Peak hours are 01:00–04:00 and 06:00–10:00 UTC, Monday through Friday, excluding
> Chinese public holidays. All other hours are off-peak, including weekends and
> Chinese public holidays in full. Off-peak rates are half of the peak rates.

| | UTC (будни, кроме праздников КНР) | Москва (UTC+3) |
|---|---|---|
| **Пик (полная цена)** | 01:00–04:00 и 06:00–10:00 | **04:00–07:00 и 09:00–13:00** |
| **Off-peak (вдвое дешевле)** | все прочие часы | 13:00–04:00 и все выходные |

В пиковые окна дорогая активность (длинные прогоны, сборки, профилирование)
приостанавливается; майнер продолжает работать и писать события в лог, чтобы не
терять статистику. Напоминания стоят на 04:00 и 09:00 МСК (свернуть) и 07:00 и
13:00 МСК (возобновить) по будням.

Оговорка: государственные праздники КНР код не отслеживает — в эти дни пик
фактически не действует, и расписание можно игнорировать.


