# ESP32-S3 AIoT

四非自动化嵌入式探索之路

基于 ESP32-S3 和 ESP-IDF 的环境监测与 AI 语音交互项目，包含 LCD 触摸界面、传感器采集、OneNET MQTT、BLE 和小智 AI 接入。

## 项目内容

- DHT11 温湿度、BH1750 光照、SGP30 TVOC/等效 CO₂ 采集与上报。
- APDS9960 手势换页，LVGL 显示与触摸交互。
- OneNET MQTT 属性上报、查询及屏幕背光控制。
- BLE 传感器数据通知与设备控制接口。
- ESP-SR 语音前端、唤醒词检测、Opus 音频及小智 AI WebSocket 通信。
- Wi-Fi 重连、网络恢复后补启云服务、传感器异常重试和 SGP30 基线保存。

当前版本已在 ESP-IDF 5.4.3 下编译通过；各外设、网络服务和音频链路仍需结合实际接线与账号完成实机验证。

## 构建与烧录

使用 ESP-IDF **5.4.3**，目标芯片为 **ESP32-S3**。当前配置使用 **8 MB Flash、Octal PSRAM**，引脚定义见 [main/main.c](main/main.c)，构建配置保存在 [sdkconfig.defaults](sdkconfig.defaults)。

1. 克隆仓库并进入工程目录：

   ```sh
   git clone https://github.com/lyl20250610/yan.git
   cd yan
   ```

2. 复制本地凭据模板，填入自己的 Wi-Fi 和 OneNET 产品、设备及密钥信息。

   PowerShell：

   ```powershell
   Copy-Item components/MY_MQTT/app_secrets.h.example components/MY_MQTT/app_secrets.h
   ```

   Linux/macOS：

   ```sh
   cp components/MY_MQTT/app_secrets.h.example components/MY_MQTT/app_secrets.h
   ```

   `app_secrets.h` 仅保留在本机，不提交到仓库。模板中的占位值必须替换后才能连接实际服务。

3. 在 ESP-IDF 5.4.3 终端中构建并烧录，将 `PORT` 替换为实际串口：

   ```sh
   idf.py build
   idf.py -p PORT flash monitor
   ```

   首次构建由组件管理器按 `dependencies.lock` 获取依赖。首次烧录应使用完整的 `flash`，同时烧录分区表、应用和语音模型。当前应用分区为 3.5 MB，模型分区为 4 MB，布局见 [partitions.csv](partitions.csv)。

## 目录

- `main/`：应用入口、任务调度和 LVGL 界面。
- `components/`：传感器、显示、触摸、Wi-Fi、BLE、MQTT 和 AI 语音模块。
- `sdkconfig.defaults`、`dependencies.lock`：可复现构建的配置与依赖版本。
- `block_diagram.html`、`driver_flow_diagrams.html`、`software_architecture.html`：项目架构参考文档；细节以当前源码为准。
