# AIL fn-API — спецификация нативного секвенсора (Stage 6.1)

Поверхность, которой игра пользуется у SBPFM.ADV (все вызовы — v2_ail.cpp,
сайты sh_call; семантика верифицирована реверсом #61/#83/#85). Нативный
секвенсор обязан покрыть РОВНО это.

| fn | Аргументы | Семантика (verified) |
|----|-----------|----------------------|
| 64 | drv, io, ? | describe/probe устройства (boot) |
| 65 | drv, ... | init драйвера |
| 66 | drv, d[0xC],d[0xE],d[0x10],d[0x12] | timer-install; тик = desc[0x14]+5 = 125 Гц у SBPFM (120+5) |
| 67 | drv | ТИК секвенсора (INT8-сетка) — сердце воспроизведения |
| 97 | drv, seq_off/seg, state_off/seg, ... (8) | register sequence → handle (XMID EVNT + state-блок) |
| 98 | drv, handle | release_sequence |
| 99 | drv | timbre cache size → [9942] |
| 9A | drv, off, seg, size | назначить кэш тембров ([9934]:[9932]) |
| 9B | drv, handle | опрос «нужен тембр»: возвращает bank<<8|patch либо 0xFFFF; результат в [9946], пара в [993E]/[9940] |
| 9C | drv, bank, patch, off, seg | загрузить тембр (адрес из скана банка 17512: [9930]:[992E]) |
| AA | drv, handle | start/resume sequence |
| AB | drv, handle | stop_sequence |
| AE | drv, handle | status (используется в wait-цикле fade/stop) |
| B1 | drv, handle, 0, 1000 | fade volume → 0 за N мс |

## Данные
- Банк (chunk 0x209+mc / GM 0x215): справочник записей по 6 байт
  {patch, bank, off_lo, off_hi, x, x} (скан до +0x3F82, sub_17512);
  off → тембр-данные внутри банка. Формат тембра — реверс по блобу (w3).
- Секвенция: XMID (FORM XDIR/CAT XMID, EVNT-события: дельты <0x80,
  note-on с VLQ-длительностью; парсер уже в assetc).
- Тик: 125 Гц (desc[0x14]+5), frame-режим: тики по счётчику вызовов.

## Оракул
V2_OPL_TRACE: `тик chip reg val` на каждую регистровую запись.
Эталонная база: assets_raw/opl_ref/*.trace (канон-58, V2_AIL_FRAME_TICKS=1)
+ MANIFEST.sha. Нативный секвенсор обязан выдавать бит-в-бит те же потоки.
