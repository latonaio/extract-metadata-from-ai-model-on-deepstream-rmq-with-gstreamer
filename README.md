# extract-metadata-from-ai-model-on-deepstream-with-gstreamer-rmq
extract-metadata-from-ai-model-on-deepstream-with-gstreamer-rmq は、GStreamer を使用して、DeepStream上でAIモデルからメタデータを抽出し、メタデータをRabbitMQへ送信するマイクロサービスです。
本マイクロサービスでは、以下のメタデータを取得できます。

- フレーム番号
- ラベル
- バウンディングボックスの座標

## 動作環境
- NVIDIA Jetson
    - Jetpack 4.6
    - DeepStream 6.0
- GStreamer 1.14
- GNU Make

注意事項: 予期せぬエラーが起きてしまう恐れがある為、ファイル構成は本レポジトリと同様にして下さい。

## 前提条件
本マイクロサービスを使用するにあたり、事前に[GStreamer](https://docs.nvidia.com/metropolis/deepstream/5.0DP/plugin-manual/index.html#page/DeepStream%20Plugins%20Development%20Guide/deepstream_plugin_details.3.01.html#)をエッジデバイス上にインストールしてください。


## 動作手順
### 必要なライブラリのインストール
以下のコマンドで必要なライブラリをインストールします。
```sh
make install
```

### 変数定義
RabbitMQのホストネーム(JetsonのIPアドレス)、ポート、バーチャルホスト、ID、パスワード、キュー名を'gstdsosdcoordrmq.c'の298~303行目に書きます。
```
char *host_name = "x.x.x.x";
int port = 32094;
char *vhost_name = "tao";
char *rmq_id = "guest";
char *rmq_pass = "guest";
char *queue_name = "peoplenet-metadata-queue-test";
```

### dsosdcoordrmqのビルド
バウンディングボックスやラベルをディスプレイに表示し、それらのメタデータをコンソールに表示するプラグインを以下のコマンドでビルドします。
```sh
make build
```

### ストリーミングの開始
以下のコマンドでストリーミングを開始します。
```sh
make start
```


## 本レポジトリにおけるGStreamerの修正部分について
本レポジトリでは、基本的に[GStreamer](https://docs.nvidia.com/metropolis/deepstream/5.0DP/plugin-manual/index.html#page/DeepStream%20Plugins%20Development%20Guide/deepstream_plugin_details.3.01.html#)のリソースをそのまま活用していますが、GStreamerのリソースのうち、[Gst-nvdsosd](https://docs.nvidia.com/metropolis/deepstream/5.0DP/plugin-manual/index.html#page/DeepStream%20Plugins%20Development%20Guide/deepstream_plugin_details.3.06.html#wwconnect_header)のリソースのみ、バウンディングボックスの座標等の設定パラメータを追加、RabbitMQへ送信するため、変更を加えています。
設定パラメータを追加した箇所は、gst-dsosdcoordrmq / gstdsosdcoordrmq.c のファイルにおける、以下の部分です。

```
if (dsosdcoordrmq->display_coord) {
    metadata.frame_number = dsosdcoordrmq->frame_num;
    metadata.label = object_meta->text_params.display_text;
    metadata.top_left.x = object_meta->rect_params.left;
    metadata.top_left.y = object_meta->rect_params.top;
    metadata.top_right.x = object_meta->rect_params.left + object_meta->rect_params.width;
    metadata.top_right.y = object_meta->rect_params.top;
    metadata.bottom_left.x = object_meta->rect_params.left;
    metadata.bottom_left.y = object_meta->rect_params.top + object_meta->rect_params.height;
    metadata.bottom_right.x = object_meta->rect_params.left + object_meta->rect_params.width;
    metadata.bottom_right.y = object_meta->rect_params.top + object_meta->rect_params.height;

    metadata_arr[m_cnt] = metadata;
    m_cnt++;
}

root = build_json(metadata_arr, m_cnt);
if (m_cnt > 0) {
    str_obj = json_dumps(root, 0);
    int reply = rabbitmq_cli_publish(cli , queue_name, str_obj);
    // printf("%s\n", str_obj);
}

```
