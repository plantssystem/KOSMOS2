
---

# 🎛️ KOSMOS2 Cloud Preset & Cloud API  
*KOSMOS2 v2 — Network‑Enabled Generative Engine*

---

## 🌩️ Cloud Preset Format (JSON Specification)

KOSMOS2 は Wi‑Fi 経由で外部プリセットをロードできる。  
プリセットは軽量 JSON 形式で構成され、生成エンジンに即反映される。

### **トップレベル構造**

```json
{
  "version": 1,
  "melody": { ... },
  "bass": { ... },
  "chord": { ... },
  "sequence": { ... },
  "global": { ... }
}
```

---

## 🎼 melody

```json
"melody": {
  "scale": "minor_pentatonic",
  "density": 0.6,
  "orbit_speed": 0.8,
  "probability": 0.7,
  "note_range": [48, 72],
  "humanize": 0.15
}
```

- **scale** — スケール名  
- **density** — ノート密度  
- **orbit_speed** — 軌道回転速度  
- **probability** — 発音確率  
- **note_range** — MIDI ノート範囲  
- **humanize** — タイミング揺らぎ  

---

## 🎸 bass

```json
"bass": {
  "pattern": "root_5th",
  "swing": 0.2,
  "octave_shift": -1
}
```

- **pattern** — root_5th / drone / arp  
- **swing** — スイング量  
- **octave_shift** — オクターブ移動  

---

## 🎹 chord

```json
"chord": {
  "mode": "triad",
  "spread": 0.4,
  "inversion": 1
}
```

- **mode** — triad / seventh / sus / power  
- **spread** — 和音の広がり  
- **inversion** — 転回  

---

## 🔁 sequence

```json
"sequence": {
  "steps": [1,0,1,0,1,0,1,0],
  "length": 8,
  "rotate": 0
}
```

- **steps** — 0/1 の配列  
- **length** — シーケンス長  
- **rotate** — 回転量  

---

## 🌍 global

```json
"global": {
  "tempo_mod": 0.1,
  "randomness": 0.2,
  "accent": 0.3
}
```

- **tempo_mod** — テンポ揺らぎ  
- **randomness** — 全体のランダム性  
- **accent** — アクセント強度  

---

# ☁️ KOSMOS2 Cloud API Specification

KOSMOS2 が Wi‑Fi 経由でプリセットを取得するための  
**軽量 REST API**。

---

## 🔌 エンドポイント一覧

| Method | Path | 説明 |
|-------|------|------|
| **GET** | **/presets** | プリセット一覧 |
| **GET** | **/presets/{id}** | プリセット JSON を取得 |
| **POST** | **/presets** | 新規プリセット作成 |
| **PUT** | **/presets/{id}** | プリセット更新 |
| **GET** | **/random** | ランダム生成プリセット |
| **GET** | **/share/{id}** | 共有リンク用プリセット |

---

## 📥 GET /presets

```json
[
  { "id": "lofi01", "name": "Lo-Fi Chill", "updated": "2026-05-20" },
  { "id": "ambient02", "name": "Deep Ambient", "updated": "2026-05-21" }
]
```

---

## 📥 GET /presets/{id}

```json
{
  "version": 1,
  "melody": { ... },
  "bass": { ... },
  "chord": { ... },
  "sequence": { ... },
  "global": { ... }
}
```

---

## 📤 POST /presets

```json
{
  "id": "my_new_preset",
  "name": "My New Preset",
  "data": { ... }
}
```

---

## 🔄 PUT /presets/{id}

```json
{
  "name": "Updated Preset",
  "data": { ... }
}
```

---

## 🎲 GET /random

```json
{
  "version": 1,
  "melody": { ... },
  "bass": { ... },
  "chord": { ... },
  "sequence": { ... },
  "global": { ... }
}
```

---

## 🔗 GET /share/{id}

```json
{
  "id": "ambient02",
  "url": "https://server/presets/ambient02",
  "preview": "Deep Ambient",
  "tags": ["ambient", "slow", "wide"]
}
```

---

## 📡 Pico 2 W からのアクセス例（擬似コード）

```c
http_get("https://server/presets/lofi01", buffer);
cJSON *root = cJSON_Parse(buffer);
apply_preset(root);
```

---

# 🔁 KOSMOS2 Cloud Polling（Pico 2 W）

目的：
- 一定間隔でクラウドからプリセットを取得  
- JSON をパース  
- KOSMOS2 の内部パラメータに反映  
- 音生成は止めない（非同期）

---

## 🧩 ポーリングタスク（Core0）

```c
void cloud_polling_task() {
    absolute_time_t next_poll = make_timeout_time_ms(5000); // 5秒ごと

    while (true) {
        if (absolute_time_diff_us(get_absolute_time(), next_poll) <= 0) {

            int len = http_get(PRESET_URL, PRESET_PATH, recv_buf, sizeof(recv_buf));
            if (len > 0) {
                // JSON パース
                cJSON *root = cJSON_Parse(recv_buf);
                if (root) {
                    // ★KOSMOS2 に適用
                    apply_preset(root);
                    cJSON_Delete(root);
                    printf("Preset updated\n");
                }
            }

            // 次のポーリング時刻をセット
            next_poll = make_timeout_time_ms(5000);
        }

        sleep_ms(10); // CPU 負荷を下げる
    }
}
```

---

## 🧠 ポイント解説

### 1. **absolute_time_t を使った正確な周期処理**
RP2350 のタイマーは安定しているので、  
**Wi‑Fi の遅延に引きずられない**。

---

### 2. **apply_preset() はスレッドセーフに**
Core1（音生成）と Core0（Wi‑Fi）が同時に触るので、  
**mutex か spinlock** を使うのが安全。

例：

```c
mutex_t preset_lock;

void apply_preset(cJSON *root) {
    mutex_enter_blocking(&preset_lock);

    // JSON → KOSMOS2 パラメータ構造体へ
    load_melody(root);
    load_bass(root);
    load_chord(root);
    load_sequence(root);
    load_global(root);

    mutex_exit(&preset_lock);
}
```

---

### 3. **ポーリング間隔は 3〜10 秒が現実的**
- Wi‑Fi の負荷  
- サーバー負荷  
- KOSMOS2 の“人格変化”の自然さ  

を考えると **5 秒** がベスト。

---

### 4. **プリセットが変わった時だけ反映する最適化**
サーバー側で `updated` タイムスタンプを返すなら：

```c
if (strcmp(last_updated, new_updated) != 0) {
    apply_preset(root);
}
```

→ 無駄な apply を避けられる。

---

# 🔥 KOSMOS2 らしい “進化ポーリング” も可能

クラウド側でランダム生成 API を作っておけば：

```c
GET /random
```

をポーリングするだけで、

> **KOSMOS2 が 5 秒ごとに人格変化する楽器**

---


↑ このように **バッククォート3つ → mermaid → 図 → バッククォート3つ**。

---

## 🎛️ KOSMOS2 Dual‑Core Architecture

```mermaid
flowchart TB

    subgraph CORE0["Core0 (RP2350) — Generative Engine + Wi‑Fi"]
        UI["UI / Encoder / CC"]
        GEN["Generative Engine<br/>• Melody Generator<br/>• Bass Generator<br/>• Chord Generator<br/>• Sequence Engine<br/>• Randomness / Orbit / Humanize"]
        WIFI["Cloud Polling (Wi‑Fi)<br/>• HTTP GET<br/>• JSON Parse<br/>• apply_preset()"]
        SCHED["MIDI Event Scheduler<br/>• USB MIDI OUT<br/>• NoteOn/Off Queue → Core1"]
    end

    subgraph CORE1["Core1 (RP2350) — Synth Engine (PRA32‑U)"]
        SYNTH["PRA32‑U Synth Engine<br/>• Osc A/B/C/D<br/>• Filter / EG / LFO<br/>• Mixer / Drive"]
        QUEUE["MIDI Queue Processor<br/>• NoteOn/Off 即時処理"]
        AUDIO["Audio Rendering<br/>• I2S + DMA (48kHz / 32bit)"]
    end

    GEN --> SCHED
    WIFI --> GEN
    SCHED --> QUEUE
    QUEUE --> SYNTH
    SYNTH --> AUDIO

---

## 1. **Core0 = ジェネレーティブエンジン**
KOSMOS2 の “人格” を作る部分はすべて Core0 に集約。

- Melody / Bass / Chord / Sequence  
- Orbit / Humanize / Randomness  
- UI / CC  
- Cloud Polling（Wi‑Fi）  
- JSON パース  
- apply_preset()  

**→ Core0 は “脳”**

---

## 2. **Core1 = シンセエンジン**
PRA32-U2 の音源部分は Core1 に固定。

- Oscillator  
- Filter  
- EG / LFO  
- Mixer  
- I2S + DMA  

**→ Core1 は “声帯”**

Core0 がどれだけ重くなっても、  
Core1 の I2S DMA は止まらない。

---

## 3. **Core0 → Core1 の通信は MIDI Queue**

- Core0 が NoteOn/Off をキューに積む  
- Core1 が高速に処理  
- 音生成は常に安定  

**→ Wi‑Fi や JSON パースが音に影響しない構造**

---

## 4. **クラウドプリセットは Core0 で apply**
Core1 は音生成に専念するため、  
プリセット反映は Core0 のみが担当。

apply_preset() は mutex で保護して  
Core1 の参照と競合しないようにする。

---

