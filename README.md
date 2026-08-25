# ReLow60 L-HE — Limited Alpha

ReLow60 L-HE は、ロープロファイルのホール効果（磁気スイッチ）キーボードです。
このリポジトリでは **Limited Alpha** 版のオープンソースアセットを公開しています。

ReLow60 L-HE is a low-profile Hall-effect magnetic-switch keyboard.
This repository hosts the open-source assets for the **Limited Alpha** release.

> 以降のリリース段階（Open Beta 等）は別リポジトリで公開します。
>
> Later release stages (e.g. Open Beta) will be published in separate
> repositories.

## 内容 / Contents

| パス / Path | 内容 / Description | ライセンス / License |
|------|-------------|---------|
| [`firmware/`](firmware/) | キーボードファームウェア（[peppapighs/libhmk](https://github.com/peppapighs/libhmk) のフォーク）<br>Keyboard firmware, a fork of [peppapighs/libhmk](https://github.com/peppapighs/libhmk) | GPL-3.0 ([`firmware/LICENSE`](firmware/LICENSE)) |

その他のアセット（ケースデータ等）は今後順次追加していきます。

Additional assets (case data, etc.) will be added here over time.

## コンフィギュレータ / Configurator

キーボードの設定には、Web ベースのコンフィギュレータ [ReConf](https://github.com/ReBotLab/ReConf) を使用します。ReConf は Web DFU によるファームウェア書き込みにも対応しています。

The keyboard is configured with [ReConf](https://github.com/ReBotLab/ReConf),
a web-based configurator. ReConf can also flash firmware via Web DFU.
