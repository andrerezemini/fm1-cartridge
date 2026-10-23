# Sources and limits of the evidence

External references used by this project. They are technical context; they neither authorize flashing hardware nor prove tests that were not executed.

| Source | Use / limit |
| --- | --- |
| [Sloop 4ec1ac8](https://github.com/isod89/sloop-fm1/tree/4ec1ac829fd2bdf2c571ea215fb3d62477d01220) (Sloop 2.7.1) | Upstream of `sloop-fm1/` and base of [`sloop-fm1-cartridge.patch`](../integrations/sloop-fm1/sloop-fm1-cartridge.patch); `firmware/src/usb.c`, `seq.c`, `main.c`, `fx.c` and the build limits. |
| [Clipper e690672](https://github.com/isod89/clipper-fm1/tree/e69067211c4690f34e494b5b896e4585163dcc2b) | DSP/hostsim origin and license documentation; porting source is not the same as running the FM-1 binary on the ESP32. |
| [Felucca 63f88ec](https://github.com/hugelton/Felucca/tree/63f88ec18b71c6ed4371d03d6f04dd93f4193ad3) (Felucca 1.6.1) with [Felucca-Engines c46e008](https://github.com/hugelton/Felucca-Engines/tree/c46e0086f69bdc26df1cd144dca9e683cc4531dd) | DSP origin of the Felucca port ([third_party/felucca](../third_party/felucca/README.md)). |
| [Melodee 398ada1](https://github.com/keremimo/melodee/tree/398ada1e09d864c12f3b778a43e45c7c2a8031b8) (Melodee 1.4.5) | DSP origin of the Melodee port ([third_party/melodee](../third_party/melodee/README.md)). |
| [JieLi AC79 AIoT SDK d179b44](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK/tree/d179b4484759423312073f5fbb232501aa491047) (SDK 1.2.1) | `include_lib/driver/cpu/wl82/asm/usb.h` (USB0 registers, endpoint count, EP4 RX/TX), `p33.h` and `charge.h` (the FM-1 charger bit), the UAC host driver; not executed on an FM-1 in this work. |
| [JieLi USB module documentation](https://doc.zh-jieli.com/AC79/zh-cn/master/module_example/peripherals/usb.html) | Host/device modes of the AC79 USB; does not cover VBUS/5 V for host. |
| [Felucca BSP 33cac69](https://github.com/hugelton/felucca-bsp/tree/33cac693903e753e8783baeed05c19cadfb04812) | FM-1 board support and its open questions on USB host and VBUS. |
| [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter) | An FM-1 on battery talks to an external host over D+/D−/GND with VBUS disconnected; third-party report. |
| FM-1 board photos ([aroum](https://github.com/aroum/fm1-custom-fw/tree/main/photos), [ip2k](https://github.com/ip2k/lunar-modulator/tree/659c3aa9b88df292d4771925f0cf431ce68f6759/photos/2026-09-29)) | USB-C sink circuit and charger path, inferred from photos; resistor values are not readable. |
| [Seeed XIAO ESP32-S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) | Memory configuration of the 8 MB boards the plain firmware profiles target (not built here). |
| [UAC host 1.5.0](https://components.espressif.com/components/espressif/usb_host_uac/versions/1.5.0/readme) and its [API](https://github.com/espressif/esp-usb/blob/6d24137e14a4f6c8138662a7a074be16a899c2c6/host/class/uac/usb_host_uac/include/usb/uac_host.h) | API and TX/RX direction; does not prove project-specific interoperability. |
| [ESP-IDF USB Host Library 5.3.6](https://docs.espressif.com/projects/esp-idf/en/v5.3.6/esp32s3/api-reference/peripherals/usb_host.html) | Host/client architecture of the ESP-IDF version the firmware is built with. |
| [USB-IF Type-C, revision 2.0](https://www.usb.org/sites/default/files/USB%20Type-C%20Spec%20R2.0%20-%20August%202019.pdf) | Source/Sink roles and CC terminations; primary source for the limit of the Rd–Rd setup, without claiming certification or a more recent revision. |

Use explicit categories in changes: **confirmed in source**, **proposed**, **implemented/tested in software** and **measured on hardware**. The ISO OUT endpoint, ASRC and mixer are implemented in the modified Sloop ([Sloop integration](../integrations/sloop-fm1/README.md)); latency, battery life and VBUS remain pending measurement. The [bench tests](BENCH_TESTS.md) record only results actually measured.
