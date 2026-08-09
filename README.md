# cpp-diskstats — Disk I/O Monitor (Linux)

Monitor de E/S de disco en **C++23** que lee `/proc/diskstats` y `/sys/block` para mostrar actividad real de los dispositivos de bloque: operaciones, throughput, % de ocupación y latencia media. Cero dependencias externas: compilación directa con `g++`.

## Features

- Tabla de dispositivos con READS/WRITES acumulados, tipo (SSD/HDD), tamaño y tasas medias desde boot
- Modo **watch** (`--watch N`): tasas delta en vivo cada N segundos (IOPS, KB/s, %busy, latencia)
- Salida **JSON** completa para scripting (`--json`)
- Ordenación por `name|r|w|iops|busy` y límite con `--top N`
- Detección de tipo de disco vía `/sys/block/*/queue/rotational` (SSD/HDD) y tamaño real
- Filtrado de dispositivos virtuales (loop, ram, zram, sr0)
- Colores ANSI (desactivables con `--no-color`)
- 18 tests de auto-verificación (`--selftest`), incluyendo parseo real de `/proc/diskstats` y validación estructural JSON

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o diskstats
```

## Usage

```bash
./diskstats                      # snapshot: tasas medias desde boot
./diskstats --watch 2            # tasas delta en vivo cada 2s
./diskstats --json               # JSON completo
./diskstats --sort busy --top 3  # los 3 discos más ocupados
./diskstats --no-color           # sin ANSI (pipes/logs)
./diskstats --selftest           # auto-verificación
```

## Ejemplo

```
Disk I/O Monitor — /proc/diskstats  (rates: avg since boot, uptime 12345s)
DEVICE   TYPE SIZE    READS   WRITES     r/s    w/s   rKB/s   wKB/s  %busy  avgIO
sda      SSD  120G    49.3M   31.5M    3991.0  2554.9  308754   259886   2.38    0.68
sda1     -    -       49.3M   31.5M    3990.5  2554.9  308724   259851   2.65    0.68
```

## Cómo funciona

| Campo | Fuente | Derivado |
|-------|--------|----------|
| READS/WRITES | `/proc/diskstats` campos 4 y 8 | conteos acumulados |
| Bytes | `sectores * 512` | — |
| r/s, w/s | Δ completions / Δt | IOPS |
| rKB/s, wKB/s | Δ sectores × 512 / Δt | throughput |
| %busy | Δ io_time_ms / Δt | % tiempo con I/O en curso (clamp 100) |
| avgIO | Δ weighted_io_time_ms / Δ ops | latencia media por operación |

## Tests

```bash
./diskstats --selftest   # 18 checks
./diskstats --json | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['summary'])"
```

## Licencia

MIT
