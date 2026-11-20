<p align="center">
  <img src="src/data/images/product_icon_32bpp-128.png" width="96" height="96" alt="ELMO_key のアイコン">
</p>

<h1 align="center">予測変換AI ELMO_key（エルモキー）</h1>

<p align="center">
  打つそばから、漢字に。PC の中の AI が前後の文脈を見て、変換を整える Windows 用の日本語入力ソフトです。
</p>

<p align="center">
  <a href="https://elmocyber12.github.io/elmo-key/"><strong>ダウンロードページ（はじめての方はこちら）</strong></a>
  ・
  <a href="https://github.com/elmocyber12/elmo-key/releases/latest">最新のリリース</a>
</p>

---

## できること
- **ライブ変換**：スペースを押さなくても、入力しているそばから漢字かな交じりに変わります。`Enter` で確定、`Space` で候補一覧、`Esc` でひらがなに戻ります。
- **AI による補正**：前後の文章を見て、PC の中の小さな AI（Zenz）が変換を直します。AI が間に合わないときや止まっているときも、ふつうの変換で入力を続けられます。
- **打った文字は外に出ない**：変換も AI も PC の中だけで動きます。インストール時に、外向きの通信を止める Windows ファイアウォールの規則も入ります。
- **キー**：`無変換` で英数（IME オフ）、`変換` でひらがな（IME オン）。設定画面のキー設定で変えられます。

## 動作環境
- Windows 11 / Windows 10（バージョン 1809 以降）の 64 ビット版（x64）
- メモリ 8 GB 以上をおすすめします（AI 込みのメモリ使用は、目安で 200 MB 台）
- インストールには管理者の許可が必要です

## インストール
[ダウンロードページ](https://elmocyber12.github.io/elmo-key/) の手順をご覧ください。
署名のないソフトのため、Windows の青い警告画面（SmartScreen）が出ます。「詳細情報」→「実行」で進めます。

Mozkey・Mozc とは内部のしくみが共通しているため、同じ PC に一緒には入れられません。入っている場合は先にアンインストールしてください。

## もとにしたもの
ELMO_key は、次のオープンソースソフトウェアをもとにしています。どちらも公式の製品ではありません。

- [Mozkey](https://github.com/koyasi777/mozkey)（koyasi777）— ライブ変換と Zenz による AI 補正を備えた Mozc のフォーク。元の README は [docs/README_mozkey.md](docs/README_mozkey.md) に残しています。
- [Mozc](https://github.com/google/mozc)（Google）

### Mozkey からの主な変更点
- 表示名を ELMO_key に変更（IME の一覧、インストーラー、バージョン情報、入力中の札、設定画面の「Zenz」「Mozc」の表記など）。内部の名前・ID・インストール先は Mozkey と同じです。
- アイコンを変更。
- 入力外観（候補の窓・入力モードの表示・入力中の札など）の初期設定を、ELMO のデザインシステム（文教版）の色に変更。
- 初期設定のキー（MS-IME 風）で、`変換` を IME オン、`無変換` を IME オフに変更。
- 初めてのインストールのとき、インストールしたユーザーの既定の入力方法を ELMO_key にする（`msiexec /i ... ELMOKEY_SET_DEFAULT_IME=0` で無効にできます）。
- 追加辞書から、元データの利用条件がはっきりしない「ニコニコ大百科・pixiv 百科事典」由来の辞書を外しました。

## ビルド
基本の手順は Mozc と同じです（[docs/build_mozc_in_windows.md](docs/build_mozc_in_windows.md)）。
追加辞書を作ってからビルドします。

```
.\tools\dictionary\prepare_daily_dictionary.ps1
cd src
bazelisk build --config oss_windows --config release_build package
```

成果物は `src/bazel-bin/win32/installer/Mozc64.msi` です。

## ライセンス
- ソースコード：BSD-3-Clause（[LICENSE](LICENSE)）。Copyright 2026 ELMO_key contributors. Portions Copyright 2026 koyasi777, 2010-2026 Google LLC.
- 同梱のソフトウェア・辞書・AI モデルのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) と、インストール先の `documents`・`licenses` フォルダーをご覧ください。
  - AI モデル Zenz v3.2 small（Miwa-Keita）：CC BY-SA 4.0 として扱っています。
  - Qt（LGPL-3.0）：使用した Qt のソースは、各リリースに添付しています。
