# 停机坪固件（网页烧录）

一个会动的鼠标底座的固件：ESP32-C3 SuperMini + 两只 MG90S 舵机 + WS2812B 灯带。放上鼠标收拢灭灯，拿起鼠标立起亮灯。非 ASUS / ROG 官方产品。

**烧录和装配说明：<https://chenwilliam1986-bot.github.io/helipad-flasher/>**（用电脑上的 Chrome 或 Edge 打开，点“烧录固件”）

- `helipad.bin`：合并好的固件，从 0x0 烧。
- `09_release/09_release.ino`：源码。Arduino，esp32 支持包 3.3.12，库 Adafruit NeoPixel 1.15.5，开发板 `esp32:esp32:esp32c3:CDCOnBoot=cdc`。
- `index.html`、`manifest.json`：网页烧录页（ESP Web Tools）。

个人原型。参数按作者的一台样机写死；这个一键版还没在第二台机器上验证。
