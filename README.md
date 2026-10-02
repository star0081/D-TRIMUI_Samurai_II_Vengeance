# Samurai II: Vengeance — порт для TrimUI Smart Pro (PortMaster и Knulli)

Порт Android-версии *Samurai II: Vengeance* (Madfinger Games) для **TrimUI Smart Pro**: стоковая прошивка **1.1.1** (PortMaster / TRIMUI_EX) и **Knulli**. Железо то же: TinaLinux, Allwinner A133P, PowerVR GE8300, экран 1280×720.

Поддерживается только сборка **1.1.4** (`com.madfingergames.SamuraiIIAll`, versionCode **101040**): Unity **4.6.3p2 Mono**, armeabi-v7a, GLES2, текстуры **ETC1**. OBB нет.

**Текущий снимок: S2-BUILD 54.**

> В репозитории **нет** игры: ни APK, ни `assets/`, ни `libunity.so` / `libmono.so` / `libmain.so`, ни `Assembly-CSharp.dll`. Их нужно положить из своей копии.

## Что работает

- Картинка на весь экран 1280×720, без растягивания игрового кадра. Заставка остаётся маленькой, её размер не трогали.
- Левый стик ходит, правый смотрит. Раскладка осей та же, что у [Shadowgun](https://github.com/star0081/D-TRIMUI_SHADOWGUN) BUILD 76, правится в `controls.txt` без пересборки.
- Звук. На стоке — ALSA. На Knulli карта занята PipeWire, поэтому запускатор говорит с ним через Pulse (`libpulse` из `host-libs`).
- **Select** открывает меню паузы. **Select+Start** выходит из порта.
- Первый рычаг в начале уровня срабатывает сам, когда персонаж подходит к нему ближе 3 метров, и персонаж поднимается вместе с мостом. Остальные рычаги игра обрабатывает сама.

## Установка
https://4pda.to/forum/index.php?showtopic=1080050&view=findpost&p=145342312

1. Свой APK 1.1.4 положить как `Data/ports/samurai2/gamedata/samurai2-1.1.4.apk` (см. `gamedata/README.txt`).
2. На ПК: `powershell -File Data/ports/samurai2/setup.ps1` — достанет `libmain.so`, `libunity.so`, `libmono.so` и `assets/`.
3. Скопировать на карту.

Сток 1.1.1:

| На карте | Откуда |
|---|---|
| `Roms/PORTS/Samurai II.sh` | запускатор, только LF |
| `Data/ports/samurai2/` | порт и извлечённая игра |

Knulli (раздел SHARE):

| На карте | Откуда |
|---|---|
| `roms/ports/Samurai II.sh` | тот же запускатор |
| `roms/ports/samurai2/` | содержимое `Data/ports/samurai2/` |

Запускатор сам берёт `samurai2` рядом с собой, если это Knulli, и `Data/ports/samurai2`, если это сток.

Лог: `logs/samurai2.log` внутри папки порта. В первой строке рантайма должно быть `S2-BUILD 54`.

## Сборка

Нужен Zig 0.13. Путь к нему записан в `Data/ports/samurai2/src/build.ps1`.

```text
powershell -File Data/ports/samurai2/src/build.ps1
```

Сокеты свои: `/tmp/s2-glbridge.sock`, `/tmp/samurai2.present.ready`. Shadowgun и NFS рядом не конфликтуют. Framebuffer стока не менять.

## Архитектура

Как NFS MW и Shadowgun на том же стоке:

1. **glbridge GLES2** — 32-битный клиент, 64-битный презентер держит окно PowerVR 1280×720.
2. **armhf sysroot** внутри порта.
3. **host-libs** — 32-bit SDL2, ALSA и Pulse.
4. **Лоадер** `samurai2_runtime` — ELF32 + Bionic-мост, JNI под Unity 4.6.

Код совместимости — MIT (см. `LICENSE`). Игра, данные и торговые марки остаются собственностью Madfinger Games и этой лицензией не покрываются.

## SHA-256 донора

| Файл | SHA-256 |
|---|---|
| APK 1.1.4 `com.madfingergames.SamuraiIIAll` | `a914d586e1d2ed3a9fdb13da1c85eaacd058462b2085810a5ebfe3ca608c4f09` |

## Благодарности

- Detoy / EapRules — ELF-лоадер и Bionic-мост
- Порт Shadowgun на TrimUI 1.1.1 — glbridge и схема PortMaster

Автор: **star0081**.

Samurai II: Vengeance является торговой маркой Madfinger Games. Это неофициальный проект совместимости.
