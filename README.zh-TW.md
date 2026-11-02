# ldac_bridge

[English](README.md) | **繁體中文**

把 Windows 的聲音用 **LDAC**（最高 990 kbps）傳到藍牙耳機。Windows 本身不支援 LDAC。

這不是核心驅動，而是一般的程式：它會接管一個藍牙控制器，自己跑一套藍牙堆疊。

```
Windows 播放 → 虛擬音效卡 (VB-CABLE) → WASAPI 擷取 → libldac 編碼 → BTstack A2DP → 藍牙控制器 (WinUSB) → 耳機
```

## 功能

- LDAC 44.1 / 48 / 88.2 / 96 kHz、990 / 660 / 330 kbps，以及簡易自適應位元率
- 耳機按鍵（播放/暫停、下一首、上一首）轉成 Windows 媒體鍵
- Windows 和耳機的音量雙向同步（AVRCP 絕對音量）
- Sony 耳機可顯示左耳、右耳、充電盒電量
- 系統匣程式：登入時自動啟動，耳機連上時跳出含電量的通知，睡眠喚醒或 USB 斷線後自動恢復
- 重新連線方式和 Windows 相同：先主動重試幾分鐘，之後等耳機自己連回來

## ⚠ 開始之前

- **藍牙控制器會從 Windows 拿走。** 必須把控制器的驅動換成 WinUSB，換了之後，Windows 原本的藍牙功能（滑鼠、鍵盤、手把、其他耳機）在這個控制器上都不能用。兩者都需要的話，請另外準備一支藍牙 dongle。這個改動可以還原，見[還原成 Windows 藍牙](#還原成-windows-藍牙)。
- **目前只測試過一種控制器**，見[支援的硬體](#支援的硬體)。
- **沒有麥克風。** 只實作 A2DP 播放，沒有通話用的耳機麥克風（HFP）。
- **延遲約 150～200 ms**，大部分在耳機內部。聽音樂沒問題；看影片時，可以用播放器的音訊延遲設定對齊（VLC、mpv、MPC-HC、PotPlayer）。
- **僅限非商業用途**，見[授權](#授權)。

## 支援的硬體

### 藍牙控制器

控制器必須被 BTstack 的 Windows WinUSB port 支援。Intel 的控制器還需要由 ldac_bridge 載入韌體。

| 控制器 | USB ID | 狀態 |
|---|---|---|
| Intel ThunderPeak，例如 Wireless-AC 9260（韌體 `ibt-18-16-1`） | `8087:0025` | ✅ 已測試 |
| 其他 Intel 控制器 | `8087:xxxx` | ❓ 需要不同的韌體檔，目前尚未支援 |
| Realtek、Broadcom、CSR、MediaTek | – | ❓ 目前尚未支援 |

很多桌機主機板和筆電內建的都是 Intel 藍牙。查看方式：裝置管理員 → 藍牙 → 你的控制器 → 內容 → 詳細資料 → 硬體識別碼，找 `VID_8087&PID_xxxx`。

### 耳機

支援 LDAC 的耳機應該都能串流。已測試：

| 耳機 | LDAC | 按鍵 | 音量同步 | 電量 |
|---|---|---|---|---|
| Sony WF-1000XM6 | ✅ | ✅ | ✅ | ✅ 左 / 右 / 充電盒 |
| EarFun Wave Pro | ✅ | 未測試 | 未測試 | – |

電量是透過 Sony 私有的控制協定讀取，所以只有 Sony 耳機會顯示。

歡迎回報其他控制器和耳機的測試結果，請開 issue。

## 安裝

### 1. 下載

到 [Releases](../../releases) 下載最新的 `ldac_bridge-*-win64.zip`，解壓縮到固定的資料夾，例如 `C:\Tools\ldac_bridge`。`firmware` 資料夾要和執行檔放在一起。想自己編譯的話，見[從原始碼建置](#從原始碼建置)。

### 2. 安裝虛擬音效卡

Windows 需要一個播放裝置來輸出聲音，ldac_bridge 會擷取這個裝置的聲音再傳給耳機。

1. 到 [VB-Audio 官網](https://vb-audio.com/Cable/)下載 **VB-CABLE**，以系統管理員身分執行 `VBCABLE_Setup_x64.exe`，然後重開機。
2. 在「設定 → 系統 → 音效」把輸出裝置設成 **CABLE Input**。
3. 執行 `ldac_bridge.exe --list-devices`，確認看得到 `CABLE Input`。

ldac_bridge 擷取的是預設播放裝置，所以音量滑桿和鍵盤音量鍵照常可用。

### 3. 把藍牙控制器交給 WinUSB

1. 下載並執行 [Zadig](https://zadig.akeo.ie)。
2. 選單 **Options → List All Devices**。
3. 選你的藍牙控制器，例如 **Intel(R) Wireless Bluetooth(R)**。**一定要確認 USB ID 和上面查到的相同**，選錯可能會讓鍵盤、滑鼠或其他 USB 裝置失效。
4. 右側驅動選 **WinUSB**，按 **Replace Driver**。
5. 關機再開機，讓控制器回到 bootloader 狀態。

### 4. 第一次連線

讓耳機進入配對模式，然後在終端機執行：

```bash
ldac_bridge.exe --name "WF-1000XM6"
```

`--name` 會選名稱包含該文字的第一個裝置。不加的話，會選找到的第一個音訊裝置。配對後會記住耳機（`last_device.txt`），之後不加參數就會自動重連。

成功時會看到類似：

```
Intel controller ready.
BTstack up and running on XX:XX:XX:XX:XX:XX.
A2DP: connected to XX:XX:XX:XX:XX:XX, querying codecs...
  remote codec: SBC
  remote codec: LDAC (seid 2), rates: 44.1k 48k 88.2k 96k
Selecting LDAC 48000 Hz stereo
A2DP: LDAC configured, 48000 Hz
A2DP: streaming LDAC. Play something on Windows. Ctrl-C to quit.
[stats] HQ 990 kbps, 9xx kbps on air, 187 pkt/s, buffer 28/30 ms, drift +12 ppm, ...
Battery: L 80% | R 79% | Case 67%
```

按 Ctrl-C 結束。

### 5. 背景執行與開機自動啟動

`ldac_tray.exe` 會在背景隱藏執行 `ldac_bridge.exe`，並在系統匣放一個圖示：

- 耳機連上時跳出通知，顯示耳機名稱和電量。
- 圖示的提示文字顯示連線狀態和電量；點圖示可以開啟記錄檔、重新啟動藍牙、結束。
- ldac_bridge 當掉、睡眠喚醒後、或控制器沒有回應時，會自動重新啟動。
- 所有輸出都寫在執行檔旁邊的 `ldac_bridge.log`。

設定登入 Windows 時自動啟動（只影響目前使用者，不需要系統管理員權限）：

```bash
ldac_tray.exe --install     # 開啟
ldac_tray.exe --uninstall   # 關閉
```

`ldac_tray.exe` 執行時，不要再另外手動執行 `ldac_bridge.exe`，兩個程式會搶同一個控制器。

Windows 顯示語言是中文時，系統匣介面為繁體中文，其他語言則為英文。

## 參數

| 參數 | 說明 |
|---|---|
| `--quality hq\|sq\|mq\|abr` | 990 / 660 / 330 kbps，或從 990 起跳的自適應。斷音時改用 `abr` 或 `sq` |
| `--latency 30` | 擷取緩衝，單位 ms，範圍 10～500（預設 30）。越低延遲越小；電腦忙碌時斷音就調高 |
| `--rate 96000` | LDAC 取樣率（預設跟擷取裝置相同，通常是 48 kHz） |
| `--device "CABLE Output"` | 指定擷取裝置（預設是預設播放裝置的 loopback） |
| `--addr XX:XX:XX:XX:XX:XX` | 連線到指定位址 |
| `--name TEXT` | 掃描時選名稱包含 TEXT 的第一個裝置 |
| `--scan` | 忽略記住的耳機，重新掃描 |
| `--list-devices` | 列出音效裝置後結束 |
| `--sony-debug` | 印出 Sony 控制協定的原始封包（電量功能除錯用） |

## 重新連線的方式

1. 啟動時或斷線後，程式會主動嘗試連線 10 次，間隔依序為 5、10、20、40、60…秒，總共約 6 分鐘。
2. 之後停止主動嘗試，只保持可被連線的狀態。耳機開機或從充電盒取出時，會自己連回最後使用的電腦。
3. 只接受已配對的耳機；其他裝置的連線或配對要求都會被拒絕，電腦也不會被搜尋到。

## 疑難排解

- **`Intel firmware setup failed`**：`firmware/` 裡沒有你的控制器對應的韌體，或控制器不在 bootloader 狀態。請完整關機再開機。
- **顯示串流中但沒聲音**：確認 Windows 的輸出裝置是 CABLE Input，且播放時 `[stats]` 那行的 `peak` 有在變動。
- **斷音**：如果 `[stats]` 那行的 `underruns` 有增加，把 `--latency` 調高（例如 50）。否則改用 `--quality abr` 或 `--quality sq`，並讓控制器的天線遠離 USB 3 連接埠和線材。
- **睡眠喚醒後藍牙沒反應**：`ldac_tray.exe` 會自動重新啟動；單獨執行 `ldac_bridge.exe` 時，程式會結束，需要再手動啟動。
- **封包記錄**：`hci_dump.pklg`（以及上一次執行的 `hci_dump.prev.pklg`）可以用 Wireshark 開啟。
- **Windows Update 之後**原本的驅動可能被裝回去，重做第 3 步即可。

## 還原成 Windows 藍牙

1. 結束 ldac_bridge（系統匣圖示 → 結束）。
2. 在「裝置管理員」→「通用序列匯流排裝置」，對你的藍牙控制器按右鍵 →「解除安裝裝置」，勾選「嘗試移除此裝置的驅動程式」。
3. 選「掃描硬體變更」或重開機，Windows 會裝回原本的驅動。
4. 耳機要在 Windows 重新配對，因為 ldac_bridge 配對時已經換掉了耳機裡的金鑰。

## 從原始碼建置

需要 [MSYS2](https://www.msys2.org) 的 UCRT64 工具鏈：

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make patch
```

然後在 Git Bash 或 MSYS2 UCRT64 shell 裡，於 repo 根目錄執行：

```bash
sh setup.sh                                # 下載固定版本的 BTstack、libldac、Intel 韌體，並套用 patches/
sh ldac_bridge/build.sh                    # 產生 ldac_bridge.exe 和 ldac_tray.exe
sh ldac_bridge/build.sh offline_test.exe   # 不需藍牙的自我測試
ldac_bridge/offline_test.exe               # 最後一行應該是 OK
```

`third_party/` 和 `ldac_bridge/firmware/` 由 `setup.sh` 產生，不放進 git。每次 push 都會由 GitHub Actions 自動建置；推送 `v*` tag 會發布 Release。

### 對 BTstack 的修改

`patches/btstack-avrcp-passthrough-release.patch` 修正 AVRCP pass-through 回應，讓它原樣回傳按鍵「放開」的位元。少了這個修正，部分耳機（Sony WF-1000XM6）會不斷重送放開指令，按鍵只有第一次有效。更新 `setup.sh` 裡的 BTstack 版本時，要確認上游是否已修正。

## 已知限制

- 音效卡和藍牙的時脈漂移，靠重取樣時加快或放慢最多 ±0.2% 來補償（`[stats]` 那行的 `drift`），聽不出來。
- 只使用 Classic 藍牙，不使用 LE Audio。
- Windows 只有「播放/暫停切換」一個鍵，如果耳機和播放程式的狀態不同步，多按一次就好。

## 授權

本 repo 的程式碼採用 [MIT 授權](LICENSE)。

編譯出來的程式連結了 [BTstack](https://github.com/bluekitchen/btstack)，它的授權**僅限非商業用途**，因此 ldac_bridge 整體不得用於任何商業目的或營利，也不得以此目的散布。LDAC 編碼使用 Sony 的 libldac（Apache 2.0）。Release 套件附有 Intel 藍牙韌體，依 Intel 的再散布授權提供。詳見 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

LDAC 是 Sony Corporation 的商標。本專案與 Sony、Intel、BlueKitchen 均無關聯，也未獲其背書。
