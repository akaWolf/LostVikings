# Система звука Lost Vikings — native AIL (актуально с #61/#79, 2026-08-06)

## Обзор

Оригинальная игра использует **AIL (Audio Interface Library, Miles)** с
драйвером **SBPFM.ADV** (Sound Blaster Pro FM): и музыка, и SFX — это
XMID-секвенции, проигрываемые FM-синтезом OPL3. В порте звуковой путь
ОДИН — **нативный**: оригинальный драйвер-блоб исполняется
8086-интерпретатором, его OPL-записи рендерит ядро Nuked OPL3, микс
уходит в SDL-устройство.

Legacy-канал (adlmidi-плеер + AudioPool, старая «decoupled mixer»
архитектура) удалён полностью (#79, коммит f62228a); от сабмодуля
`src/adlmidi` компилируется только `chips/nuked/nukedopl3.c`.

## Архитектура

```
game code (seg000 AIL-цепи: 17561 init / 176bd start / 177bb SFX /
           17912·1782a·1787f стопы / 108c8 mute / 178f1 fade)
  │  CALLF sub_1cXXX  (seg002-стабы: mov ax,fn; jmp sub_1bec2)
  ▼
seg002 sub_1bec2 — lookup (sub_1be8a) → dx:ax = far ptr хендлера В БЛОБЕ
  │  orig: push dx / push ax / retf  (= far-jump в блоб)
  ▼
loc_1bed6 МОСТ (#61): аргументы = 10 слов кадра [sp+4..] →
  v2_ail_orig_bridge(fn_off, args) → интерпретатор → ax/dx → RETF
  ▼
src/sdl/v2_ail_interp.cpp — 8086-интерпретатор .ADV-блоба
  два инстанса g_ails[2]: 0 = shadow (v2-мир), 1 = real (m2c-мир,
  плоская карта: параграф 0 → m2c-арена); recursive_mutex = модель
  PUSHF/CLI оригинала
  ▼
src/sdl/v2_native_opl.cpp — ОДИН Nuked OPL3 (модель DOSBox sbpro2):
  порты 0x220/1 = банк 0, 0x222/3 = банк 1 (reg|0x100), NEW-бит 0x105,
  4-op Connection Select 0x104 (тимбры SBPFM — четырёхоператорные!);
  mixer 0x224/5 — shadow-модель (игра программирует только Mic-пробу
  fn65, тома Master/FM не трогает); ×2 гейн к DOSBox-референсу
  ▼
src/sdl/play.cpp — SDL-устройство: sound_init открывает AUDIO_S16SYS,
  my_audio_callback = v2_nopl_mix + диагностики (SOUND-UNDERRUN /
  SOUND-CLIP / SOUND-CB-OVERRUN, NO_AUDIO_WORK, V2_AUDIO_DUMP-тап)
```

## Тикер (fn67)

- DOS: INT8 от PIT (частота из дескриптора драйвера).
- V2_ONLY: real-time по аудио-часам (потреблённые семплы устройства =
  стенное время) с cap догона: долг >64 тиков (~0.5 с) → сдвиг базы =
  честная ПАУЗА вместо пачечного отыгрыша; плюс pump внутри
  vsync-wait-петель (все блокирующие циклы игры проходят через них —
  ровно где DOS продолжал получать INT8). `V2_AIL_FRAME_TICKS=1` —
  детерминированный frame-режим для реплеев.
- default/verify: ВСЕГДА frame-accumulator (125/60 за pump), pump только
  из FRAME_BEGIN + blocking-tick — оба инстанса тикаются парой под одним
  локом → байт-идентичное состояние драйвера в обоих мирах; DS-verify
  сравнивает драйверные стейты как обычные байты DS.

## Ключевые механики драйвера (доказаны дизасмом блоба)

- fn64 desc / fn65 detect / fn66 init / fn97 register / fn9B→fn9C
  timbre-цикл / fnAA start / fnAB stop / fn98 deferred release /
  fnAE status / fnB1 fade (громкость → 0 за N мс; [state+0x2C] ← 0x64
  в момент вызова — старт рампы).
- Тимбр-кэш fn9C: слоты 0..191; каталог patch@0xB8B / bank@0xACB /
  busy@0xC4B / off@0x94B; LRU 0x64B/0x7CB; лимиты [0xD0F]=0xE00, [0xD11];
  тимбры 14 Б (2-op) / 25 Б (4-op); fn66→0x19BE сброс каталога и
  drum-map (REP STOSB 0xFF @0x12B9).
- Тик-арифметика 0x3382: [+0x30] += [+0x32], порог 0x64, tempo-slew
  [+0x32]→[+0x34].
- C0-хендлер 0x26C9: [12A9+ch]=prog → 0x15E9 → [1289+ch]=слот;
  fetch-гейт 0x351F по [2A2B]&0x80.

## DS-состояние звука (все адреса типизированы в v2_gamestate)

- Указатели стейтов @0x9920 → пять XMID-блоков по 0x208 @0x9950..0xA378.
- Handle-слоты `[si−0x66F4]` → 0x990C.. (si=0 музыка, 2..8 SFX);
  seq-слоты `[si−0x66EA]` → 0x9916..
- Music-диспетчер off_3285A (@0xA37A, 5 CS-адресов; индекс = snd_type
  [25B7], sub_17749).
- Track→chunk-id таблица @0xA384: sub_1775d читает
  `[track*2−0x5C7C]` (wrap), прибавляет [86B8] и грузит чанк трека в
  seg_sound через sub_10982.
- Конфиг карты: [86B6] (sound==8) / [86B8] (music==3) — из хвоста
  DATA.DAT.

## Юнит-мир (fn-test)

Гейт закрыт наглухо: `v2_fntest_running` → native off; на входах
sub_176bd / sub_177bb стоят fntest-RETN-леса (иначе глухой fn9B-стаб
эхоит 0x9B≠0xFFFF — вечный timbre-цикл на синтетическом DS; репро —
sub_154bf). Десять юнитов звуковой семьи — SKIPPED-PORT-NATIVE; покрытие
семьи = DS-паритет default-мира по всему интерпретируемому драйверу +
V2_ONLY-смок + 15-сценарный набор.

## Диагностика

- `V2_AIL_TRACE=1` — пошаговый трейс интерпретатора.
- `V2_OPL_TRACE=<f>` — поток «тик банк рег знач» (сравнение с DOSBox-DRO,
  методика — memory reference-dosbox-instrumentation).
- `V2_AUDIO_DUMP=<f>` — сырой s16le device-bound микс («что слышал юзер»).
- `NO_AUDIO_WORK=1` — колбэк отдаёт тишину (перф-изоляция).
- `tests/native_ail_check.sh` — V2_ONLY-смок: boot + music + SFX,
  faults=0, слот-кап, ≥1000 OPL-записей.

## Данные

- .ADV-блоб = чанк 0x1CC DATA.DAT (LZSS; size+1-канон — python/lv_lzss.py:
  underflow-выход, резать по dx==0 = потерять хвост).
- Банк тимбров — соседний чанк; валидация ресурса — magic «Copy»
  в байтах 2–5.
- XMID-треки уровней: chunk-id из таблицы @0xA384 (+[86B8]).
