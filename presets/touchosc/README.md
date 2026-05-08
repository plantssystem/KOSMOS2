
---

# 🎛 KOSMOS2 Controller（TouchOSC）

このフォルダには **KOSMOS2 Controller（TouchOSC版）** のレイアウトファイル  
**`KOSMOS2_Controller.tosc`** と、その説明書である **README.md** が含まれています。

KOSMOS2 Controller は、ジェネレーティブ音響システム **KOSMOS2** を  
**TouchOSC からリアルタイム操作するための専用 UI** です。

- 4 パート（Melody / Bass / Chord / Sequence）を直感的にコントロール  
- Density / Pitch / Speed / Scale / Volume を即時変更  
- ライブ演奏・即興生成に最適化  
- RP2040（Raspberry Pi Pico）上の KOSMOS2 ファームウェアと完全連携

---

## 🎹 1. ファイル構成

```
presets/
└─ touchosc/
      ├─ KOSMOS2_Controller.tosc   ← TouchOSC レイアウト
      └─ README.md                 ← この文書
```

---

## 🎛 2. 概要

KOSMOS2 Controller は、KOSMOS2 の生成アルゴリズムを  
**手で触るように操作できるインターフェース**です。

TouchOSC 上で以下のパラメータを操作できます：

- **Density（密度）**  
- **Pitch（ピッチバイアス）**  
- **Speed（進行速度）**  
- **Scale（スケール切替）**  
- **Master Volume（音量）**  
- **Randomizer（ランダマイズ）**

これらはすべて **MIDI CC** として RP2040 の KOSMOS2 に送信されます。

---

## 🎚 3. MIDI マッピング

| UI | 機能 | MIDI CC |
|----|------|---------|
| Density | ノート密度・発音確率 | CC20 |
| Pitch | ピッチ方向の傾き | CC21 |
| Speed | 再生速度 | CC22 |
| Scale | スケール切替 | CC23 |
| Master Volume | 全体音量 | CC7 |
| Randomizer | 内部状態ランダマイズ | CC24（任意） |

KOSMOS2 の Core0 はこれらの CC を受信し、  
Core1 の PRA32‑U2/M シンセへリアルタイム反映します。

---

## 🌀 4. UI の説明

### **4.1 XY Pad（Density / Pitch）**

KOSMOS2 の動きを決める最重要 UI。

- **X軸：Density**  
  - ノート密度  
  - ユークリッドパターンの濃さ  
  - 発音確率  
- **Y軸：Pitch**  
  - メロディの上下方向の傾向  
  - ベースラインの方向性  
  - フレーズの重心

XY Pad ひとつで **音楽の流れ全体を操れる**ように設計されています。

---

### **4.2 Speed（スピード）**

- 再生速度・進行速度を調整  
- 右へ動かすと速く、左へ動かすと遅くなる  
- KOSMOS2 の「歩く速さ」を決めるパラメータ

---

### **4.3 Scale（スケール切替）**

CC23 によりスケールを切り替えます。

- Hirajoshi  
- Miyakobushi  
- Insen  
- Pentatonic  

スケール変更は KOSMOS2 の生成結果に大きく影響します。

---

### **4.4 Master Volume（音量）**

- CC7 によるマスター音量  
- TouchOSC のフェーダーで直感的に操作可能

---

### **4.5 Randomizer（ランダマイズ）**

KOSMOS2 の内部状態をランダム化します：

- フレーズ方向  
- リズム構造  
- オクターブ配置  
- ベロシティ傾向  
- ノート選択傾向

ライブ中の変化付けに最適です。

---

## 🔧 5. 使用方法

1. TouchOSC に `KOSMOS2_Controller.tosc` を読み込む  
2. KOSMOS2（RP2040）を USB‑MIDI デバイスとして接続  
3. TouchOSC の MIDI OUT を KOSMOS2 に向ける  
4. 各 UI を操作すると、KOSMOS2 の生成音がリアルタイムに変化します

---

## 🧩 6. 対応環境

- **RP2040（Raspberry Pi Pico）**  
- KOSMOS2 ファームウェア（Core0：UI/MIDI、Core1：PRA32‑U）  
- TouchOSC（iOS / Android / Windows / macOS）

---

## 📜 7. ライセンス

- KOSMOS2 Controller（TouchOSC レイアウト）は **MIT License**  
- 商用利用・改変・再配布すべて可能

---
