# ldac_bridge

把 Windows 的系統聲音用 **LDAC** 傳到藍牙耳機。不是核心驅動，而是一般程式：

```
Windows 播放 → 虛擬音效卡 (VB-CABLE) → WASAPI 擷取 → libldac 編碼 → BTstack A2DP → Intel 藍牙 (WinUSB) → 耳機
```

- 藍牙堆疊：[BTstack](https://github.com/bluekitchen/btstack)（非商業用途免費）
- LDAC 編碼器：AOSP `libldac`（Apache 2.0）
- 支援 44.1 / 48 / 88.2 / 96 kHz、990 / 660 / 330 kbps，以及簡易自適應位元率（ABR）

## 這台電腦的狀況

| 項目 | 值 |
|---|---|
| 藍牙晶片 | Intel `VID_8087&PID_0025`（ThunderPeak，hw_variant 0x12） |
| 韌體 | `firmware/ibt-18-16-1.sfi`、`.ddc`（來自 linux-firmware） |
| 測試耳機 | Sony WF-1000XM6 `58:18:62:72:48:95`、EarFun Wave Pro `70:5A:6F:6D:F7:9E` |

## 一、建置

在 Git Bash 裡（需要 MSYS2 UCRT64 的 gcc，這台已經有），於 repo 根目錄執行：

```bash
sh setup.sh                                # 下載固定版本的 BTstack、libldac、Intel 韌體，並套用 patches/
sh ldac_bridge/build.sh                    # 產生 ldac_bridge/ldac_bridge.exe
sh ldac_bridge/build.sh offline_test.exe   # 不需藍牙的自我測試
ldac_bridge/offline_test.exe
```

`third_party/` 和 `ldac_bridge/firmware/` 都由 `setup.sh` 產生，不放進 git。

`offline_test.exe` 會測 LDAC 各種取樣率與品質下的封包大小和位元率，以及 WASAPI 擷取。最後一行應該印出 `OK`。

## 二、安裝虛擬音效卡（必要）

這台電腦唯一的播放裝置就是藍牙耳機。藍牙交給 ldac_bridge 之後，Windows 就沒有播放裝置了，所以需要一張虛擬音效卡：

1. 到 VB-Audio 官網下載 **VB-CABLE**，以系統管理員身分執行 `VBCABLE_Setup_x64.exe` 安裝，然後重開機。
2. 在「設定 → 系統 → 音效」把輸出裝置設成 **CABLE Input**。
3. 用 `ldac_bridge.exe --list-devices` 確認看得到 `CABLE Input`。

## 三、把 Intel 藍牙切換給 WinUSB

> ⚠ 切換後 Windows 的藍牙會消失，耳機只能透過 ldac_bridge 使用。還原方式見第五節。

1. 下載 [Zadig](https://zadig.akeo.ie) 並執行。
2. 選單 **Options → List All Devices**。
3. 下拉選單選 **Intel(R) Wireless Bluetooth(R)**，並確認 USB ID 顯示 **`8087 0025`**。這一步一定要核對，不要選錯裝置。
4. 右側驅動選 **WinUSB**，按 **Replace Driver**。
5. 建議重開機（完整關機再開更好），讓晶片回到 bootloader 狀態。

## 四、執行

先讓耳機進入**配對模式**（WF-1000XM6：放進充電盒、打開蓋子，長按耳機觸控區直到進入配對）。

```bash
ldac_bridge.exe --name WF-1000XM6
```

第一次會掃描、配對、連線，之後會記住耳機（`last_device.txt`），直接執行 `ldac_bridge.exe` 就會自動重連。耳機斷線後每 5 秒會重試一次。

成功時會看到類似：

```
Intel controller ready.
BTstack up and running on XX:XX:XX:XX:XX:XX.
A2DP: connected to 58:18:62:72:48:95, querying codecs...
  remote codec: SBC
  remote codec: LDAC (seid 2), rates: 44.1k 48k 88.2k 96k
Selecting LDAC 48000 Hz stereo
A2DP: LDAC configured, 48000 Hz
A2DP: streaming LDAC. Play something on Windows. Ctrl-C to quit.
LDAC: encoder ready, 48000 Hz, HQ 990 kbps, mtu ...
[stats] HQ 990 kbps, 9xx kbps on air, ...
```

常用參數：

| 參數 | 說明 |
|---|---|
| `--quality hq\|sq\|mq\|abr` | 990 / 660 / 330 kbps，或從 990 起跳的自適應（斷音時用 `abr` 或 `sq`） |
| `--rate 96000` | 指定 LDAC 取樣率（預設跟擷取裝置相同，通常是 48 kHz） |
| `--device "CABLE Output"` | 指定擷取裝置（預設是 Windows 預設播放裝置的 loopback） |
| `--addr 58:18:62:72:48:95` | 直接連指定位址 |
| `--scan` | 忽略記住的耳機，重新掃描 |
| `--sony-debug` | 印出 Sony 控制協定的原始封包（電量功能除錯用） |

除錯時，`hci_dump.pklg` 可以用 Wireshark 開啟，看完整的藍牙封包。

## 背景執行與開機自動啟動

`ldac_tray.exe` 會在背景隱藏執行 `ldac_bridge.exe`，並在系統匣放一個圖示：

- **連上時跳出通知**，內容包含耳機名稱和電量，例如「已連上 WF-1000XM6」「左 80%｜右 79%｜充電盒 67%」。
- **圖示的提示文字**顯示目前狀態和電量；點圖示會開選單，可以開啟記錄檔、重新啟動藍牙、結束。
- **自動重新啟動**：`ldac_bridge.exe` 結束或藍牙控制器沒有回應（例如睡眠喚醒後 USB 被重置）時，會自動重新啟動。
- 所有輸出都寫在 `ldac_bridge/ldac_bridge.log`。

設定登入時自動啟動（寫入目前使用者的「啟動」登錄機碼，不需要系統管理員權限）：

```bash
ldac_bridge/ldac_tray.exe --install     # 開啟自動啟動
ldac_bridge/ldac_tray.exe --uninstall   # 取消自動啟動
```

`ldac_tray.exe` 執行時，不要再另外手動執行 `ldac_bridge.exe`，兩個程式會搶同一個藍牙裝置。

### 重新連線的方式（和 Windows 相同）

1. 啟動時或斷線後，程式會主動嘗試連線 10 次，間隔依序為 5、10、20、40、60…秒，總共約 6 分鐘。
2. 之後就停止主動嘗試，只保持「可被連線」的狀態。耳機開機或從充電盒取出時，會自己連回最後一台主機，也就是這台電腦。
3. 只接受已配對的那副耳機；其他裝置的連線或配對要求都會被拒絕，電腦也不會被其他裝置搜尋到。

## 五、還原成 Windows 藍牙

1. 關掉 ldac_bridge。
2. 開「裝置管理員」→「通用序列匯流排裝置」→ 對 **Intel(R) Wireless Bluetooth(R)** 按右鍵 →「解除安裝裝置」，勾選「嘗試移除此裝置的驅動程式」。
3. 選「掃描硬體變更」或重開機，Intel 驅動就會裝回來。
4. 耳機需要在 Windows 重新配對，因為 ldac_bridge 配對時已經換掉了耳機裡的金鑰。

## 對 BTstack 的修改

- `third_party/btstack/src/classic/avrcp_target.c`：pass-through 回應原本會漏掉「放開」的位元（bit 7），導致 WF-1000XM6 不斷重送放開指令，按鍵只有第一次有效。已修正為原樣回傳整個 operation byte，修改內容在 `patches/btstack-avrcp-passthrough-release.patch`，由 `setup.sh` 自動套用。之後如果更新 `setup.sh` 裡的 BTstack 版本，要確認上游是否已修正。

## 已知限制

- **電量**：透過 Sony 私有的 RFCOMM 協定讀取左耳、右耳和充電盒的電量，例如 `Battery: L 80% | R 79% | Case 67%`。連線時讀一次，之後每 60 秒查詢一次，耳機主動通知時也會更新，但只有數值改變時才會印出。耳機放在充電盒裡時會顯示 `in case`。已在 WF-1000XM6（第二版協定）上確認。
- **耳機按鍵（AVRCP）**：播放/暫停、下一首、上一首、停止會轉成 Windows 媒體鍵，由 Windows 送到目前的媒體播放程式（Spotify、瀏覽器、播放器等）。Windows 只有「播放/暫停切換」一個鍵，所以如果耳機和 Windows 的狀態不同步，多按一次就好。
- **音量（AVRCP 絕對音量）**：Windows 的音量（CABLE Input 的滑桿、鍵盤音量鍵、靜音）會同步到耳機，在耳機上調音量也會同步回 Windows。連線時以 Windows 的音量為準。音量由耳機端處理，送出去的訊號維持滿刻度。`[stats]` 那行的 `peak` 是擷取到的峰值電平，可以用來確認 Windows 音量有沒有同時衰減擷取到的聲音。
- **沒有麥克風（HFP）**。
- **只支援 Classic 藍牙**：WF-1000XM6 的 LE Audio 不會被使用。
- **延遲約 100～200 ms**（50 ms 擷取緩衝 + LDAC + 耳機緩衝），看影片可能稍有不同步。
- **時脈補償**很簡單：音效卡和藍牙的時脈漂移靠丟棄或補零處理，大約每幾分鐘可能出現一次極短的爆音。
- **Windows Update** 可能把 Intel 驅動裝回去，到時重做第三節即可。
