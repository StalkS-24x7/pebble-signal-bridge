# Signal Bridge for Pebble

Unofficial Signal companion app for Pebble / Core Time 2 watches running PebbleOS.

> **Not affiliated with Signal Messenger LLC.**  
> Uses Android's standard notification listener API — no Signal protocol access.

## Features

- View incoming Signal messages on your wrist
- Full conversation thread view — up to 5 messages per contact, newest first
- View image attachments (scaled to 120×120, Pebble 64-colour palette)
- Image LRU cache — last 3 received images available without re-fetching
- Reply with 10 canned messages or voice dictation
- Timeline pins — Signal messages appear in the Pebble timeline from the watchface
- Persistent message store — conversations survive watch app restarts
- Unread indicator dot per thread
- Works on GrapheneOS (no Google Play Services required)

## Architecture

```
Signal ──► NotificationListenerService ──► Gadgetbridge ──► Core Time 2
                 (Android APK)                (BT)          (PebbleOS app)
```

---

## Prerequisites

| Tool | Purpose |
|------|---------|
| WSL2 + Ubuntu 22.04 | Pebble SDK environment |
| Android Studio (Windows) | Build the companion APK |
| Gadgetbridge (F-Droid) | Bridges watch ↔ phone over Bluetooth |
| Pebble SDK 4.5 | Compile the watch app |

---

## Part 1 — Pebble SDK Setup (WSL2)

### 1.1 Enable WSL2

Open PowerShell as Administrator:

```powershell
wsl --install -d Ubuntu-22.04
```

Restart when prompted, then complete the Ubuntu user setup.

### 1.2 Install dependencies in WSL2

```bash
sudo apt update && sudo apt install -y \
  python2 python2-dev python3-pip \
  libssl-dev libffi-dev \
  gcc-arm-none-eabi \
  npm nodejs \
  git curl wget

# pip for Python 2
curl https://bootstrap.pypa.io/pip/2.7/get-pip.py -o /tmp/get-pip.py
sudo python2 /tmp/get-pip.py
```

### 1.3 Install the Pebble SDK

```bash
# Download Pebble SDK 4.5 (maintained via Rebble)
mkdir -p ~/pebble-dev
cd ~/pebble-dev
wget https://github.com/aveao/peacock-sdk/releases/download/sdk-4.5/sdk-core.tar.bz2
tar -xjf sdk-core.tar.bz2

# Install pebble-tool
sudo python2 -m pip install pebble-tool --pre \
  --index-url https://d1skbh0er50y37.cloudfront.net/pypi/

echo 'export PATH="$PATH:$HOME/.local/bin"' >> ~/.bashrc
source ~/.bashrc
```

> **Tip:** If `pebble-tool` install fails, try the community-maintained fork:
> `pip2 install git+https://github.com/pebble-dev/pebble-tool.git`

### 1.4 Verify installation

```bash
pebble --version
# Expected: pebble-tool 4.5-rc3 or similar
```

### 1.5 Install the Pebble emulator (optional but useful)

```bash
pebble sdk install latest
```

---

## Part 2 — Build the Watch App

### 2.1 Clone and build

```bash
# In WSL2
cd ~/pebble-dev
git clone <this-repo> signal-bridge
cd signal-bridge/watch-app

pebble build
```

Output: `build/signal-bridge.pbw`

### 2.2 Run in emulator

```bash
pebble install --emulator emery    # Pebble Time 2 — 200×228 rectangular
```

### 2.3 Install on your Core Time 2

Copy `build/signal-bridge.pbw` to your Windows filesystem
(`/mnt/c/Users/micha/...`) then sideload via Gadgetbridge:

1. Open Gadgetbridge on your phone
2. Long-press your watch entry → Install app
3. Select `signal-bridge.pbw`

---

## Part 3 — Build the Android Companion App

### 3.1 Open in Android Studio

1. File → Open → select `android/` folder
2. Let Gradle sync complete

### 3.2 Build and install

Connect your Pixel 8 Pro via USB, enable USB debugging in developer options, then:

```
Build → Build Bundle(s)/APK(s) → Build APK(s)
```

Or install directly: **Run → Run 'app'**

### 3.3 Grant notification access

1. Open **Signal Bridge** app on your phone
2. Tap **Grant Notification Access**
3. Find **Signal Bridge** in the list and enable it
4. Return to app — status should show **GRANTED**

---

## Part 4 — Connect Gadgetbridge

1. Install **Gadgetbridge** from F-Droid
2. Pair your Core Time 2 via Gadgetbridge (not system Bluetooth settings)
3. Signal Bridge will automatically use Gadgetbridge as the Bluetooth bridge

---

## Usage

### On the watch

| Button | Action |
|--------|--------|
| Up/Down | Scroll message list |
| Select (message list) | Open conversation |
| Select (conversation) | Open reply screen |
| Select (reply screen) | Send canned reply or dictate |
| Back | Return to previous screen |

### Canned replies

Edit `watch-app/src/main.c` — find the `CANNED[]` array near the top
and replace with your preferred phrases. Rebuild and reinstall.

### Images

Images are automatically sent after a message if Signal includes one in
the notification. You will see an 80×80 colour image above the message
body. Not all messages include the image in the notification (depends on
Signal's notification style).

---

## Troubleshooting

**Watch app shows "No messages"**  
→ Check Gadgetbridge is connected and Signal Bridge has notification access.  
→ Check the watch UUID in Gadgetbridge logs matches `a8e3c7f2-...`.

**Images never appear**  
→ Signal may not include images in notifications on your Android version.  
→ Check Logcat for `SignalListener: Signal notification` entries.

**Replies fail silently**  
→ Signal's notification may have expired. Reply cache is cleared on app restart.  
→ Try replying within 60 seconds of receiving the message.

**Build fails: Python2 not found**  
→ Run `sudo apt install python2` and retry.

---

## Regenerating the App UUID

You must generate a unique UUID if you fork/distribute your own build:

```bash
python3 -c "import uuid; print(uuid.uuid4())"
```

Update in **both**:
- `watch-app/package.json` → `pebble.uuid`
- `android/.../PebbleCommunicator.kt` → `APP_UUID`

---

## Publishing

- **Rebble App Store** (watch app): https://rebble.io/appstore — submit the `.pbw`
- **F-Droid** (Android APK): Submit via https://gitlab.com/fdroid/fdroiddata
- **GitHub Releases**: Tag a release and attach both the `.pbw` and `.apk`

---

## Contributing

Pull requests welcome. Key areas for improvement:

- Group chat sender disambiguation
- Message history persistence (currently session-only)
- Bi-directional emoji support
- iOS companion (very limited — notification interception is not possible)

## License

GPL-3.0 — see [LICENSE](LICENSE)
