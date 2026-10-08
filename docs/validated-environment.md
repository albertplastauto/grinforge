# Проверенная среда (baseline)

Все результаты по солверу, stratum-клиенту и приёму шар получены **на этой
конфигурации**. Числа хешрейта, поведение драйвера и сам факт приёма шары пулом
привязаны к среде: при смене драйвера, CUDA или компилятора их нужно перепроверять.

## Зафиксированные версии

| Компонент | Версия |
|---|---|
| GPU | NVIDIA GeForce RTX 4060 Ti, 8188 MiB, compute capability **8.9** (Ada), VBIOS 95.06.26.00.52 |
| **Драйвер NVIDIA** | **617.14** (CUDA UMD 13.4) |
| CUDA Toolkit | **13.4**, V13.4.59 (`nvcc`) |
| Компилятор | MSVC **14.51.36231**, `cl` 19.51.36257 (Visual Studio 18 Community 2026) |
| Windows SDK | 10.0.26100.0 |
| CMake | 4.3.1-msvc1 |
| Ninja | 1.13.2 |
| Nsight Compute | 2026.3.0 (26.3.0.0) |
| ОС | Windows 11 Pro, 10.0.26200 |
| Python (для независимых проверок) | 3.12.10 |
| Архитектура сборки | `-arch=sm_89`, Windows x64 |
| Ревизия проекта на момент проверки | `1b3eabf` |

Проверить эти значения на другой машине:

```bat
nvidia-smi --query-gpu=name,driver_version,vbios_version,memory.total,compute_cap --format=csv
nvidia-smi | findstr /C:"CUDA Version"
nvcc --version
cl 2>&1 | findstr Version
cmake --version
```

## Что именно проверено на этой среде

| Что | Результат | Чем проверено |
|---|---|---|
| BLAKE2b-512 | совпадает с независимой реализацией на Python | `solver_bench --selftest`, `tools/check_keys.py` |
| siphash-2-4 на устройстве и на хосте | совпадение 64/64 | `solver_bench --device-check` |
| Заголовок: `pre_pow` 238 Б + big-endian nonce | `OK` | `solver_bench --selftest` |
| Поиск циклов Cuckatoo | 6-циклы на C20/P6, 42-циклы на C29, все проходят `grin_verify` | `solver_bench_tiny`, `solver_bench29` |
| C32 lean-солвер целиком | ~1.0 млн рёбер после тримминга, 16.9 с на граф, без зависаний | длительный прогон |
| Хешрейт | **0.053–0.055 GPS** при 28–76 Вт | `grinforge --tune`, `/stat` |
| Stratum | login, job, keepalive, submit | живой пул grin.2miners.com:3030 |
| **Приём шары пулом** | **принята**, `method=submit result=ok`, `acc=1` | лог майнера + публичный API пула |
| Независимое подтверждение | `currentHashrates {"32":0.07}` и воркер в `/api/miners` | API пула |
| Телеметрия | NVML: температура, мощность, частоты, VRAM, кулер | `grin_monitor`, `/stat` |
| Управление картой | power limit и блокировка частоты работают при правах администратора; undervolt невозможен (`numBaseVoltages=0`); кулер блокирован драйвером | `--tune`, `--install-gpu-profile` |
| Подбор режимов GPU | хешрейт не зависит от профиля (±1 %), оптимум по GPS/Вт — потолок ~2500 МГц | `grinforge --tune 25` |
| Крипто-примитивы | проверены против спецификации, не против собственного кода | `docs/root-cause.md` |

## Правило

> **Сменился драйвер, CUDA или компилятор — прогони проверку заново.**
> Не потому что что-то обязательно сломается, а потому что все числа выше
> относятся именно к этой среде, и без перепроверки они перестают быть
> доказательством.

Практический минимум после обновления драйвера (2–3 минуты, GPU освободить):

```bat
:: 1. Крипто-примитивы и модель заголовка
build\cmake\solver_bench.exe --selftest

:: 2. siphash на устройстве против хоста (ловит смену поведения компилятора)
build\cmake\solver_bench.exe --pre-pow-file build\job.txt --device-check

:: 3. Поиск циклов на маленькой и на реальной конфигурации
build\cmake\solver_bench_tiny.exe --nonce-start 15 --nonce-count 6 --ntrims 16
build\cmake\solver_bench29.exe  --nonce-start 85 --nonce-count 3 --ntrims 68

:: 4. Хешрейт по профилям
build\cmake\grinforge.exe --tune 25 --bench-pre-pow build\job.txt

:: 5. Майнинг и реальная шара
build\cmake\grinforge.exe --pool grin.2miners.com:3030 ^
    --user <кошелёк>.RIG1 --pass x --allow-address <кошелёк>
:: затем в логе: "submit accepted by the pool"
```

Ожидаемые результаты: `--selftest` → `passed (0 failures)`, `--device-check` →
`agree (0/64 mismatches)`, окна поиска → `verify=OK` на каждом решении,
`--tune` → 0.053–0.055 GPS на всех профилях.

## Драйвер: решение по обновлению

По состоянию на 8 октября 2026 приложение NVIDIA предлагало обновление. **Решено не
обновлять:** текущая конфигурация полностью проверена, карта поддерживается, а
обновление означало бы перезагрузку и потерю проверенной базы. Выигрыша для майнинга
не ожидается, а отказ lolMiner (`Unsupported device or driver version`) новым драйвером
не лечится — это его собственная поддержка C32.

Если обновление понадобится (например, из-за исправлений безопасности), действовать
так: зафиксировать текущую версию (617.14), сохранить установщик для отката, обновить,
перезагрузить, прогнать набор команд выше и сверить хешрейт с таблицей.
