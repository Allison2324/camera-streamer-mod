# Camera-streamer: синхронізація цифрового зуму (ScalerCrop) з кадрами

## Мета
Забезпечити **100% точне** визначення того, **який саме ScalerCrop (цифровий кроп/зум)** застосовано **до кожного кадру**, що йде на клієнта, без блокування HTTP та без впливу на FPS.

Підтвердження має бути не “команда прийнята”, а **“кадр уже вийшов із новим кропом”**.

Результат у термінах часу захоплення кадру:
- `last_before_us` — час захоплення останнього кадру зі старим crop
- `first_after_us` — час захоплення першого кадру з новим crop
- `cmd_id` — монотонний ідентифікатор команди

Важливо: `last_before_us/first_after_us` мають бути в **тій самій часовій шкалі**, що й `captured_time_us` кадрів.

---

## Принцип роботи
1. `GET /option?ScalerCrop=...` встановлює control у чергу libcamera і **відразу** повертає `cmd_id`, не чекаючи кадру.
2. На кожному dequeued кадрі (completed request) з metadata зчитується **фактично застосований** `ScalerCrop`.
3. Якщо applied crop змінився відносно попереднього кадру — фіксується apply-event:
   - `last_before_us = prev_frame_us`
   - `first_after_us = current_frame_us`
   - `applied_crop = crop із metadata`
   - подія прив’язується до актуальної pending-команди
4. Клієнт робить легкий polling через `/status` і отримує стан `crop_sync`:
   - поточний applied crop
   - pending-команду
   - останню apply-подію
   - історію команд (ring buffer) зі статусами `pending/applied/superseded`

---

## Поведінка API

### `/option`
- `GET /option?ScalerCrop=...`
  - відповідає одразу (без очікування кадру)
  - повертає `cmd_id` (у тілі відповіді)

### `/status`
У кожному `devices[i]` додано блок `crop_sync`, який містить:
- `current_applied` — applied crop для останнього обробленого кадру
- `last_frame_us` — timestamp останнього кадру, який врахував `crop_sync`
- `pending` — поточна pending команда (або `false`)
- `last_apply_event` — остання подія застосування (або `false`)
- `history` — список останніх команд (обмежений)

---

## Стани та структури даних

### Команда (cmd)
Зберігає:
- `cmd_id` (монотонний)
- `submit_time_us`
- `desired` (що запитали)
- `state`: `pending | applied | superseded`
- `superseded_by_cmd_id` (якщо перекрита новою командою)
- `last_before_us`, `first_after_us`, `applied` (заповнюються при apply)

### Apply-event
Зберігає:
- `cmd_id`
- `last_before_us`
- `first_after_us`
- `applied`

---

## Головна гарантія “100% точно”
Джерело істини — metadata completed request у libcamera:

- `request->metadata().get<Rectangle>(controls::ScalerCrop)`

Це **реально застосований** ScalerCrop для конкретного кадру.

Для кожного dequeued кадру:
- оновлюється `buffer->captured_time_us`
- з metadata витягується `ScalerCrop` у `buffer->crop_*`
- викликається `device_crop_sync_on_frame(dev, captured_time_us, crop_valid, x, y, w, h)`

Критично: `captured_time_us` використовується як стабільний frame-id для синхронізації.

---

## Змінені файли та призначення

### 1) `device/libcamera/buffer.cc`
**Навіщо:** у dequeue кадра є completed request і metadata — тут можливо витягти applied ScalerCrop і timestamp кадру.

**Що змінено:**
- Зчитування `ScalerCrop` із metadata (Rectangle) і заповнення:
  - `buf->crop_valid, crop_x, crop_y, crop_width, crop_height`
- Виклик `device_crop_sync_on_frame(...)` одразу після заповнення полів для кадру

---

### 2) `device/libcamera/device.cc`
**Навіщо:** коли приходить команда `ScalerCrop` через `/option`, вона має отримати `cmd_id` та потрапити у crop_sync як pending.

**Що змінено:**
- Під час `libcamera_device_set_option(...)` для `ScalerCrop` додано виклик:
  - `device_crop_sync_submit(dev, valid, x, y, w, h)`
Це створює команду з новим `cmd_id`, а якщо попередня команда ще pending — помічає її `superseded`.

---

### 3) `cmd/camera-streamer/http.c`
**Навіщо:** повернути `cmd_id` клієнту без блокувань.

**Що змінено:**
- Після успішного `device_set_option_string(...)` для `ScalerCrop` у відповідь додається `cmd_id`:
  - `device_crop_sync_take_last_cmd_id(dev)`

---

### 4) `cmd/camera-streamer/status.cc`
**Навіщо:** клієнт має отримувати підтвердження та стан через легкий polling.

**Що змінено:**
- У JSON статусу для кожного device додано `crop_sync`
- Стан формується через snapshot-функцію:
  - `device_crop_sync_snapshot(...)`

---

### 5) `device/libcamera/libcamera.cc`
**Навіщо:** усунути build-помилку `designator order ...` (C++ + `-Werror`) через designated initializer структури `device_hw_t`.

**Що змінено:**
- Прибрано designated initialization
- Використано `memset` + поелементні присвоєння функцій-хендлерів

Це робить код незалежним від порядку полів у `struct device_hw_s`.

---

### 6) `device/crop_sync.*` (нове)
**Навіщо:** централізований стан синхронізації команд і applied crop, мінімальна логіка прив’язки “pending → applied”.

**Ключові функції:**
- `device_crop_sync_submit(...)`
- `device_crop_sync_on_frame(...)`
- `device_crop_sync_snapshot(...)`
- `device_crop_sync_take_last_cmd_id(...)`
- `device_crop_sync_detach(...)` (виклик при закритті device)

---

## Вимоги до продуктивності
- HTTP: без очікування кадру, відповідь миттєва
- Кадрова обробка: коротке порівняння crop + рідкісний запис події під коротким mutex
- Історія обмежена (ring buffer), без росту пам’яті

---

## Перевірка роботи
1. `/status` містить `crop_sync` у кожному `devices[i]`
2. `/option?ScalerCrop=...` повертає `cmd_id`
3. Через кілька кадрів у `/status`:
   - `last_apply_event` стає не `false`
   - у `history` команда переходить у `applied`
4. Якщо нова команда прийшла до застосування попередньої:
   - попередня стає `superseded`
   - нова лишається `pending` до apply

---

## Рекомендації для клієнта
1. Після `GET /option?ScalerCrop=...` зберегти `cmd_id`.
2. Регулярно опитувати `/status`.
3. Визначати межі застосування:
   - кадри з `captured_time_us <= last_before_us` мають старий crop
   - кадри з `captured_time_us >= first_after_us` мають новий crop
4. Використовувати timestamps як frame-id в одній часовій шкалі.

---
