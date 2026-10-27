# Remote AC v2: SRC kiri atas dan data sesuai sumber yang dipilih

Target remote: ESP32 DevKit klasik, Arduino-ESP32 3.x, ArduinoJson 7.
Node: gunakan firmware `Node_Sensor_Fix` untuk ESP32-C3 Super Mini dan baterai GPIO0. Board S3 trial masih didukung.

## Pasang

1. Ekstrak ZIP. Buka `Remote_AC_Reference_Fix_v2/Remote_AC_Reference_Fix_v2.ino`.
2. Pastikan `fuzzy_logic.h` proyek ikut berada di folder sketch ini; file tersebut sudah tersedia di workspace saat pembaruan C3/baterai. Jangan menggantinya dengan mock pengujian.
3. Folder sketch sudah berisi `protocol.h`, `button_hold.h`, `sensor_reference.h`, dan `remote_node_link.h`. Semua perlu berada bersama `.ino`. `protocol.h` menambahkan pesan baterai tanpa mengubah format suhu lama.
4. Gunakan library Adafruit GFX, SSD1306, SHT31 (dengan `readBoth`), IRremoteESP8266, PubSubClient, serta ArduinoJson 7.
5. Untuk remote ESP32 DevKit **flash 4 MB**, pilih **Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**. Firmware BLE + TLS/MQTT + IR berukuran sekitar 1,89 MB sehingga tidak muat pada partisi default 1,3 MB. Pastikan ukuran flash board sesuai; pengaturan Huge APP ini tidak menyediakan OTA. Upload ke ESP32 DevKit remote, bukan ke node C3. Serial Monitor: 115200 baud.
6. Tidak perlu menghapus provisioning Wi-Fi/room yang sudah benar. Namespace NVS masih `ac_config`.
7. Pada DevKit dengan flash 4 MB, gunakan Partition Scheme `Huge APP (3MB No OTA/1MB SPIFFS)` agar firmware BLE/Wi-Fi/TLS/IR muat. Flash Size harus sesuai ukuran fisik board. Tidak perlu Erase All Flash untuk perubahan ini.

Pin tetap: OLED SDA21/SCL22, SHT31 remote SDA32/SCL33, IR GPIO4, pushbutton GPIO13 ke GND.

## Perintah aplikasi yang sudah ada

Firmware hanya subscribe ke topic command canonical:

```text
room/<room_id>/ac/command
```

Topic legacy `room/<room_id>/command` tidak lagi digunakan. Backend juga hanya
melakukan satu publish ke topic canonical sehingga setiap command diproses satu
kali. `<room_id>` tetap berasal dari hasil provisioning; jangan menebak bahwa
nomor ID database sama dengan nama ruangan.

Pilih node:

```json
{"command":"SET_CONTROL_SOURCE","value":"USER_NODE"}
```

Pilih remote:

```json
{"command":"SET_CONTROL_SOURCE","value":"REMOTE"}
```

Alias remote yang dikenali: `REMOTE_SENSOR`, `MAIN_REMOTE`, `MAIN_SENSOR`, `MAIN`, `INTERNAL`. Alias node: `NODE`. Nilai tidak dikenal ditolak agar typo tidak mengubah referensi.

Firmware juga menerima `{"command":"set_sensor_reference","reference":"node"}` atau `"remote"`, tetapi **gunakan kontrak aplikasi yang sudah ada**. Tidak perlu menambah endpoint atau tabel backend.

Frontend mengirim nilai `REMOTE` dan `USER_NODE` melalui perintah yang sama. Backend tidak perlu diubah selama meneruskan perintah pada topic tersebut.

Mode Fuzzy memakai kontrak command yang sama:

```json
{"command":"SET_MODE","value":"FUZZY"}
```

Untuk kembali manual gunakan value `MANUAL`. Aplikasi dan backend menggunakan
key `value`; backend menerbitkan payload MQTT dengan `command`, `value`, dan
`timestamp`. Firmware tetap dapat membaca key `mode` dari APK/backend lama,
tetapi jalur normal sekarang selalu memakai `value`. Status MQTT `ac/status`
juga menyertakan `mode`, `ac_mode`, `fan_speed`, dan `swing` di samping field
lama `power` dan `suhu`.

## Okupansi, Fuzzy, dan notifikasi ruangan kosong

- Saat mode `FUZZY` aktif, AC otomatis menyala ketika okupansi berubah menjadi lebih dari 0. Inferensi pertama dijalankan segera menggunakan sensor aktif yang dipilih.
- `SET_MODE=FUZZY` hanya mengubah mode dan mem-publish ACK status; perintah tersebut tidak langsung menembakkan setelan IR lama. Remote menghitung output Fuzzy terlebih dahulu lalu mengirim tepat satu perintah IR hasil inferensi.
- Pada mode `MANUAL`, perubahan okupansi tidak menyalakan atau mematikan AC secara otomatis.
- Pada kedua mode, okupansi 0 hanya memulai timer lima menit ketika AC sedang menyala. Mematikan AC membatalkan timer agar tidak ada notifikasi ruangan kosong saat AC sudah OFF.
- Event diterbitkan ke topic `room/<room_id>/notification`:

```json
{"type":"ROOM_EMPTY","people_count":0,"duration_seconds":300,"message":"Ruangan tidak mendeteksi pengguna selama 5 menit"}
```

- Frontend juga memiliki fallback notifikasi dalam aplikasi ketika halaman remote sedang terbuka. Agar notifikasi muncul saat aplikasi ditutup/background, backend perlu subscribe topic tersebut dan mengirim push notification, misalnya melalui FCM.

## Telemetri suhu yang tetap kompatibel

Topic tetap: `room/<room_id>/telemetry`.

```json
{"suhu":24.45,"kelembaban":69.10}
```

- Nama field tetap `suhu` dan `kelembaban`, bukan contoh `temperature`/`humidity` di percakapan.
- Baterai node memakai paket ESP-NOW `MSG_BATTERY=10`. Paket diperiksa MAC/ukuran/sequence dan tidak memperbarui timestamp suhu. Node lama tetap kompatibel.
- Dua nilai selalu berasal dari **satu sumber aktif yang sama**. Tidak menggabungkan suhu node dengan kelembapan remote.
- OLED dan input suhu fuzzy menggunakan snapshot aktif yang sama dengan payload MQTT.
- Aplikasi membaca `sensor.temperature` dan `sensor.humidity` sebagai snapshot aktif tersebut. Aplikasi tidak memiliki buffer node kedua; pemilihan sumber dilakukan satu kali di firmware remote.
- OLED menampilkan suhu 1 desimal, RH bilangan bulat seperti sebelumnya; MQTT 2 desimal. Perbedaan digit tampilan hanya pembulatan.
- Telemetri dikirim langsung saat referensi berubah/data node baru masuk, dan heartbeat tetap sekitar 5 detik. Perubahan terlihat di aplikasi setelah backend/frontend memproses pesan.
- Setelah MQTT reconnect, nilai sumber aktif dikirim lagi.
- Pilihan sumber disimpan ke NVS, tidak ditulis ulang pada setiap paket.
- Sensor node sudah mengirim suhu terkoreksi; remote tidak mengalibrasinya lagi.
- Koefisien polinomial sensor remote tetap mengikuti firmware lampiran: A=-0.079333553, B=4.910880485, C=-47.819913147. Moving average remote tetap 3 sampel. Mesin/rule fuzzy eksternal tidak diganti.

## Sumber yang tidak tersedia

`sensor_reference.h` mengatur batas data node dan remote sebesar 20000 ms.

| Kondisi | OLED | MQTT dan fuzzy |
|---|---|---|
| Pilih remote, sensor valid | `SRC:REMOTE` | Sensor remote |
| Pilih node, paired dan valid | `SRC:NODE` | Sensor node |
| Pilih node, belum paired | `SRC:NODE`, `T:--C`, `RH:--%`, `PAIR NODE` | Tidak mengirim data sensor/fuzzy |
| Pilih node, belum ada sampel | `SRC:NODE`, angka `--`, `WAIT NODE` | Tidak mengirim data sensor/fuzzy |
| Pilih node, invalid atau umur >=20 detik | Otomatis menjadi `SRC:REMOTE` | Memakai sensor remote dan mengirim event `NODE_OFFLINE` |
| Node pulih | Tetap `SRC:REMOTE` | Node dapat dipilih lagi dari aplikasi |
| Pilih remote, tetapi sensor remote invalid | `SRC:REMOTE`, `NO DATA`, angka `--` | Pengiriman sensor dan inferensi fuzzy dihentikan; setpoint terakhir tidak diubah |

Ketika node yang sebelumnya terhubung tidak mengirim `HELLO`, telemetry, maupun baterai selama 20 detik, firmware menyimpan pilihan `REMOTE`, memublikasikan status terbaru, dan mengirim event `NODE_OFFLINE`. Semua tiga jenis paket memperbarui heartbeat sehingga satu paket yang terlambat/hilang tidak langsung memutus node. Batas waktu diperlukan karena ESP-NOW tidak menyediakan sinyal khusus ketika saklar node dimatikan. Saat baterai node mencapai **3,30 V atau kurang**, sumber juga dipindahkan ke remote dan event `NODE_BATTERY_LOW` dikirim. Ambang dipulihkan pada 3,45 V agar notifikasi tidak bolak-balik karena noise. Jika sensor remote rusak, firmware tidak otomatis berganti ke node.

Status `ac/status` kini menambahkan `control_source`, `node_paired`, `node_connected`, dan `node_battery_low`. Riwayat pairing tetap `true` ketika node hanya offline sehingga aplikasi tetap dapat menampilkan menu referensi.

SRC sekarang berada di koordinat (0,0), kiri atas. Status AC di kanan atas, suhu/RH di baris kedua, jumlah orang dan status data di baris terakhir. Pergantian sumber dan paket valid menandai layar dirty sehingga tidak hanya label yang diperbarui.

HELLO hanya menandai kontak radio, bukan pembaruan suhu. Paket telemetri duplikat/sequence lama tidak memperpanjang umur data. Sensor invalid tidak menghasilkan suhu palsu 0 C atau NaN ke fuzzy.

Karena schema backend tidak ditambah, status SRC/tidak ada data tidak ikut dikirim. Aplikasi hanya menerima suhu/RH terpilih yang valid. Saat node dipilih tetapi belum valid, dashboard dapat tetap menampilkan nilai terakhir sampai ada telemetri baru; penanda data lama di aplikasi bergantung pada timestamp/status backend yang sudah ada. Jangan menganggap angka lama sebagai bukti paket node berhasil masuk.

## Topic baterai node

Baterai dipublikasikan mandiri ke `room/<room_id>/node/battery`, sehingga tetap sampai ke backend ketika SHT31 node/remote rusak atau suhu/RH aktif tidak tersedia. Topic suhu lama tidak berubah.

Payload valid:

```json
{"version":1,"valid":true,"millivolts":3800,"percent":65}
```

Payload unavailable/stale:

```json
{"version":1,"valid":false,"millivolts":null,"percent":null}
```

Remote memublikasikan setiap paket baterai baru, termasuk pembacaan yang eksplisit invalid, lalu memublikasikan invalid ketika data valid berubah menjadi stale setelah 20 detik dan setelah MQTT reconnect. Dengan begitu backend dapat membedakan perangkat hidup dengan pembacaan unavailable dari koneksi yang berhenti mengirim. Pesan tidak memakai retained flag; backend harus memakai waktu penerimaan server dan timeout sendiri. Log remote tersedia sebagai `[NODE_BATTERY]`. Kontrak backend lengkap ada di `../Node_Sensor_Fix/BACKEND_BATTERY_PROMPT.md`.

## Pairing yang diperlukan

1. Hidupkan node memakai firmware `Node_Sensor_Fix`.
2. Aplikasi mengirim `{"command":"START_PAIRING","value":"OPEN"}`.
3. Remote membuka discovery 60 detik, mengirim kandidat `{"mac":"..."}` pada topic lama `room/<room_id>/pairing/candidate`.
4. Aplikasi mengirim `{"command":"START_PAIRING","value":"APPROVE"}`.
5. Remote mengirim OFFER dengan session yang sesuai. Hanya ACCEPT dari kandidat yang telah disetujui boleh diproses.
6. Remote menyimpan MAC node ke NVS **sebelum** COMMIT. COMMIT diulang jika ACCEPT diulang karena respons hilang. Node menyimpan pasangan seperti biasa.
7. Node yang baru disetujui otomatis menjadi sumber `USER_NODE`. Tunggu telemetri pertama. Pilihan masih dapat diubah kembali ke `REMOTE` dari aplikasi.

`REJECT`/`CLOSE` menutup pairing. Hanya satu node dipasangkan untuk remote ini. Lakukan unpair untuk mengganti node.

**Pemulihan pasangan lama tanpa reset node:** versi lama tidak menyimpan MAC pasangan di remote. Node bisa melaporkan MAC delivery OK, sementara remote baru menolak karena belum ada pasangan. Di v2, buka pairing (`START_PAIRING`/`OPEN`) dan tunggu node mengirim HELLO/TELEMETRY. MAC node tersebut akan muncul sebagai kandidat pada topic kandidat yang sama. Setelah pengguna mengirim `APPROVE`, remote menyimpan MAC dan menggunakan sampel kandidat yang masih segar. Node yang masih paired tidak perlu melakukan ulang OFFER/ACCEPT atau direset. MAC tidak otomatis dipercaya tanpa persetujuan pengguna. Jika node masih mengirim ke MAC remote yang berbeda, lakukan reset node dan pairing biasa ke remote yang benar.

Untuk unpair node: `{"command":"START_PAIRING","value":"UNPAIR"}`. Perintah disimpan dan dikirim segera, lalu diulang saat node mengirim HELLO/TELEMETRY, termasuk setelah remote restart. Data node dicabut segera dari referensi aktif. Protokol v1 belum menyediakan ACK UNPAIR; marker/MAC yang dicabut tetap disimpan sampai pairing baru/factory reset, sehingga pesan bisa dicoba lagi tanpa mengklaim node sudah pasti menghapus pasangan.

Jika aplikasi mendeteksi node lama sudah offline tetapi remote masih menyimpan pasangan, aplikasi dapat mengirim `{"command":"START_PAIRING","value":"REPLACE"}`. Remote menghapus referensi pasangan lama, kembali ke sensor remote, lalu membuka jendela pairing selama 60 detik. Perintah ini dipakai oleh menu **Tambahkan Node Sensor** setelah riwayat node lama ditemukan.

Perintah `{"command":"UNPAIR"}` yang berdiri sendiri tetap berarti factory reset **remote** seperti kode asal; jangan tertukar.

Penerimaan ESP-NOW memakai queue dan worker terpisah: callback hanya memvalidasi panjang/header lalu menyalin paket. Worker membalas node tanpa menunggu loop MQTT/TLS. Shared sensor data memakai critical section singkat; MQTT dan OLED hanya berjalan di loop utama.

## Reset dan Wi-Fi dari perbaikan sebelumnya

- Pushbutton normally-open GPIO13 ke GND, `INPUT_PULLUP`. Tahan 3 detik menghapus `ac_config` dan restart ke provisioning BLE. Reset juga menghapus pilihan sumber dan pasangan node. Lepaskan tombol setelah reset agar latch menunggu pelepasan selesai.
- Sebelum NVS dihapus, remote yang sedang tersambung MQTT mengirim event `DEVICE_FACTORY_RESET` ke `room/<room_id>/notification`. Backend harus menandai ESP32 unpaired dan mengirim push FCM agar aplikasi kembali ke dashboard otomatis. Jika MQTT sedang offline, event fisik tidak dapat dikirim; log Serial menjelaskan kondisi ini.
- Node sensor tidak memakai pushbutton reset. Remote membalas telemetry dan HELLO dengan ACK aplikasi. Setelah lima pengiriman tanpa ACK, node masuk discovery; remote menerima pairing ulang otomatis hanya jika MAC sama dengan pasangan yang sudah tersimpan.
- Username Enterprise dibaca dari NVS saat boot, tidak hanya disimpan oleh BLE.
- Password angka tetap string JSON: `"pass":"00123456"`, tidak dikirim sebagai number.
- PEAP default; TTLS MSCHAPv2 opsional. Metode UNNES-ID perlu sesuai konfigurasi kampus, belum terkonfirmasi oleh pengujian jaringan nyata.
- Wi-Fi retry tanpa loop restart; task tombol tetap aktif saat TLS mencoba koneksi.
- Kredensial MQTT dan pengaturan TLS mengikuti sketch sebelumnya.

## Trial yang disarankan

1. Pair node, lalu letakkan remote dan node pada kondisi suhu/RH berbeda.
2. Pilih REMOTE: OLED dan aplikasi menampilkan sensor remote.
3. Setelah pairing, pastikan keduanya menampilkan sensor node secara otomatis, serta fuzzy memakai suhu node jika mode fuzzy aktif.
4. Pilih REMOTE lagi: keduanya kembali ke remote.
5. Pilih USER_NODE dan restart remote: pilihan dan pasangan tetap tersimpan. Sebelum paket node pertama, OLED `WAIT NODE` dan angka `--`.
6. Matikan node: sekitar 20 detik sejak heartbeat terakhir, SRC berpindah dan tersimpan sebagai `REMOTE`; aplikasi tetap menampilkan pasangan node sebagai offline. Push dapat membutuhkan beberapa detik tambahan untuk melewati MQTT, queue, FCM, dan sistem Android. Hidupkan lagi, tunggu heartbeat, lalu node dapat dipilih kembali.
7. Unpair ketika node tidur: saat bangun, node menerima perintah. Setelah unpair, node harus kembali ke discovery.

## Verifikasi

Uji host C++ mengompilasi sketch asli ini dengan mock Arduino/ESP-IDF dan fuzzy dummy yang berada **di luar folder sketch**. Pengujian memeriksa dispatch perintah, sumber yang ketat tanpa fallback, koordinat SRC/suhu/RH, konsistensi OLED/MQTT/input fuzzy, NVS, approval pairing, pemulihan pasangan lama tanpa reset node, paket pendek/MAC asing, session, timeout, sensor invalid, dan mailbox unpair. Uji terpisah memeriksa overflow `millis()` dan sequence.

Catatan mock di atas berasal dari versi sebelumnya. Mock JSON hanya menangani subset JSON untuk pengujian dispatch; ini bukan verifikasi parser ArduinoJson. Mock NVS/radio/task bukan pengujian timing, sertifikat, flash, atau hardware nyata. Pada pembaruan C3/baterai, uji compile-time state baterai lolos (stale, sequence, rollover, unknown, unpair, dan pemisahan timestamp suhu). Build asli node C3 berhasil menggunakan Arduino-ESP32 3.3.4. MQTT/ESP-NOW end-to-end bersama aplikasi dan pengukuran ADC tetap perlu diuji pada perangkatmu.

## Log diagnosis di remote (115200 baud)

- `[SRC_CMD] value=USER_NODE`: perintah source benar-benar masuk ke remote.
- `[SOURCE] dipilih=NODE aktif=NODE status=OK`: pembacaan valid node benar-benar dipakai.
- `[NODE_PAIR] tersimpan=TIDAK`: belum ada MAC node yang diterima sebagai pasangan.
- `[NODE_DROP] MAC pasangan belum tersimpan...`: paket radio masuk, tetapi perlu OPEN lalu APPROVE dari aplikasi.
- `[NODE_DROP] MAC berbeda...`: node pengirim bukan pasangan yang tersimpan.
- `[NODE_RX] DITERIMA | T=... RH=...`: remote menerima dan menyimpan suhu/RH node; bukan sekadar sukses lapisan MAC di node.
- `[NODE_RX] SENSOR_INVALID`: payload masuk, tetapi nilai/sensor_valid tidak valid.
- `[NODE_DIAG] RX=... bad=... queue_full=... no_pair=... wrong_mac=... accepted=...`: counter callback/worker untuk membedakan tidak ada radio, salah protokol, queue penuh, pairing, dan sampel yang diterima.

Saat NODE dipilih, counter dicetak sekitar setiap 5 detik. Callback Wi-Fi hanya menghitung/queue; semua Serial log dilakukan di worker/loop. ACK telemetri memakai struktur ACK protokol v1 yang sama; node sebelumnya tetap kompatibel dan tidak harus diubah. Log sukses node sebelumnya tetap menyatakan MAC delivery, bukan jaminan payload sudah dipakai aplikasi.

Referensi API:
- https://github.com/adafruit/Adafruit_SHT31/blob/master/Adafruit_SHT31.h
- https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32/api-reference/network/esp_now.html
