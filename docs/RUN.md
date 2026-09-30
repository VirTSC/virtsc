# Running VirTSC
This guide sets up the scripts that start and connect to your IntelliSTAR 1 VM. You should have finished building VirTSC for your platform first ([Debian](build/DEBIAN.md), [Windows](build/WINDOWS.md), [macOS](build/MACOS.md) or [WSL](build/WSL.md)).

This is rather involved, so I'll try to hold your hand as much as possible through this. 

Each step explains what to do, then gives the commands for each platform. **Expand the section for your platform.**

# Step 1: Creating the VM folder
First things first - have an unmodified, raw IntelliSTAR 1 disk image handy. You need a folder to hold the image, the scripts, and the VM's logs. In this guide we use `~/i1` on Linux and macOS, and `C:\IS1` on Windows, with the image at `weatherscan.img` inside it.

<details>
<summary><b>Debian/macOS/WSL</b></summary>

Create the folder, and place your image at `~/i1/weatherscan.img`:
```bash
mkdir ~/i1
```
Create FIFO files for input video and audio:
```bash
mkfifo ~/i1/is1-in-v ~/i1/is1-in-a
```
</details>

<details>
<summary><b>Windows</b></summary>

The build guide already created `C:\IS1`. Place your image at `C:\IS1\weatherscan.img`.

The build is finished, so **everything from here runs from a normal, non-elevated PowerShell window** unless a step says otherwise. The Windows build does **not** support the input FIFOs used on Linux, so there are no `is1-in-v` or `is1-in-a` files to create.

**Optional: enabling WHPX acceleration.** See note under the "Windows" section of Step 3 for more information. To enable WHPX, open **PowerShell as Administrator** and run:
```powershell
DISM.exe /Online /Enable-Feature /FeatureName:HypervisorPlatform /All
```
**Reboot Windows after enabling the feature.** The [QEMU WHPX documentation](https://www.qemu.org/docs/master/system/whpx.html) is available for reference.

</details>

# Step 2: Choosing a disk format
### You now have a choice to make. You can use the raw disk image for the VM, or you can convert it to a qcow2 file.

- **Raw image (EASIEST):** the VM writes straight to your image.
- **qcow2 snapshot file:** the VM writes to a compressed copy, and the original raw image is kept read-only as a recovery copy.

Converting may take a while. If it fails, there's a good chance the raw disk image has bad sectors on it; use the raw image instead. *If you know what you're doing,* you can also boot an existing **vmdk** image by setting the format to `vmdk` in Step 3.

<details>
<summary><b>Debian/macOS/WSL</b></summary>

**Raw:** make sure the image is writable:
```bash
chmod 666 ~/i1/weatherscan.img
```
**qcow2:** convert your source image using the **qemu-img** binary from your build folder, and create an overlay to not write over your image:
```bash
# Convert your existing disk to QCOW
qemu-img convert -p -f raw -O qcow2 -c ~/i1/weatherscan.img ~/i1/weatherscan-pure.qcow2
# Make it read only
chmod 444 ~/i1/weatherscan-pure.qcow2
# Create an overlay to use
qemu-img create -f qcow2 -b ~/i1/weatherscan-pure.qcow2 -F qcow2 ~/i1/weatherscan.qcow2
```
</details>

<details>
<summary><b>Windows</b></summary>

**Raw:** nothing to do.

**qcow2:** make sure the VM is **stopped**, then convert the image using **`qemu-img.exe`** from your build and make the original read-only:
```powershell
$env:PATH = 'C:\msys64\mingw64\bin;C:\vtsc-build\qemu-install;' + $env:PATH

# Convert your existing disk to QCOW
qemu-img.exe convert -p -f raw -O qcow2 -c `
  C:\IS1\weatherscan.img `
  C:\IS1\weatherscan-pure.qcow2

# Make it read only
Set-ItemProperty C:\IS1\weatherscan-pure.qcow2 -Name IsReadOnly -Value $true

# Create an overlay to use
qemu-img create -f qcow2 -b C:\IS1\weatherscan-pure.qcow2 -F qcow2 C:\IS1\weatherscan.qcow2
```
</details>

# Step 3: Creating the startup script
Great. Now we're going to create a shell script that you can easily launch your IntelliSTAR 1 VM with. **Before saving it, check the two settings at the top:** You need to change `qemu-system-i386` to its complete path, if you did not add the **qemu-is1/build** folder to your system's PATH.

- The **image path**. If you converted to qcow2, point it at `weatherscan.qcow2`.
- The **format**: `raw`, `qcow2`, or `vmdk`.

If you change your mind about the disk format later, just edit those two settings.

Every script includes **`-device is1-clock`**, which lets the IS1 read your host's time and CPU clock rate. The guest guide installs **is1clock**, a small program that uses it to keep the IS1's clock accurate. Without it, the IS1's clock can run slow, and **renderd** eventually stops with **`Panic: Time drifted too much`**. Keep **`-rtc base=utc,clock=host`** as it is: is1clock relies on it.

<details>
<summary><b>Debian/WSL</b></summary>

Change `qemu-system-i386` to its complete path if you did not add the **qemu-is1/build** folder to your system's PATH.
```bash
cat > ~/i1/run.sh <<'EOF'
#!/bin/sh
IMG="$HOME/i1/weatherscan.img"
FORMAT=raw

# The logs, QMP socket and FIFOs live next to this script.
cd "$(dirname "$0")"
exec qemu-system-i386 \
  -name IS1 \
  -machine pc,acpi=off \
  -global i440FX.agp=on -global i440FX.agp-aperture-size=128M \
  -global piix3-ide.force-bus-master=on \
  -accel kvm -cpu pentium3 -m 512 -smp 1 \
  -drive file=$IMG,format=$FORMAT,if=ide,cache=writeback -boot c \
  -vga cirrus \
  -netdev user,id=net0,net=10.100.102.0/24,host=10.100.102.1 \
  -device i82557b,netdev=net0 \
  -netdev user,id=net1,net=10.0.2.0/24,host=10.0.2.2,hostfwd=tcp:127.0.0.1:2222-10.0.2.15:22 \
  -device e1000-82545em,netdev=net1 \
  -rtc base=utc,clock=host \
  -serial file:./serial.log \
  -qmp unix:./qmp.sock,server,nowait \
  -audiodev sdl,id=audio0 \
  -device thunderstorm,id=tsc0,present=on,version=0x011a0012,\
input=bars,input-pipe=./is1-in-v,input-audio=./is1-in-a,output=./is1-output,\
stamp=host-ns,tstamp=host-s,timecode=utc,audio=silence,\
audiodev=audio0 \
  -device is1gl,id=is1gl0,mmio=0xfed10000,iobase=0x520 \
  -device is1-clock \
  -display gtk,zoom-to-fit=on,gl=off
EOF
```

You should now have a **run.sh** script in your folder. Let's make it executable.
```bash
chmod +x ~/i1/run.sh
```

</details>

<details>
<summary><b>Windows</b></summary>

Save the following as **`C:\IS1\Run-VirTSC.ps1`**:
```powershell
$BuildRoot = 'C:\vtsc-build'
$DiskImage = 'C:\IS1\weatherscan.img'
$DiskFormat = 'raw'
$VmDirectory = Split-Path -Parent $DiskImage

$env:PATH = "$BuildRoot\osmesa\bin;C:\msys64\mingw64\bin;$BuildRoot\qemu-install;$env:PATH"

$Qemu = "$BuildRoot\qemu-install\qemu-system-x86_64.exe"
$QemuArgs = @(
    '-L', "$BuildRoot\qemu-install\share",
    '-name', 'IntelliSTAR 1',
    '-machine', 'pc,acpi=off',
    '-global', 'i440FX.agp=on',
    '-global', 'i440FX.agp-aperture-size=128M',
    '-global', 'piix3-ide.force-bus-master=on',
    '-accel', 'tcg',
    '-cpu', 'pentium3',
    '-m', '512',
    '-smp', '1',
    '-drive', "file=$DiskImage,format=$DiskFormat,if=ide,cache=writeback",
    '-boot', 'c',
    '-vga', 'cirrus',
    '-netdev', 'user,id=net0,net=10.100.102.0/24,host=10.100.102.1',
    '-device', 'i82557b,netdev=net0',
    '-netdev', 'user,id=net1,net=10.0.2.0/24,host=10.0.2.2,hostfwd=tcp:127.0.0.1:2222-10.0.2.15:22',
    '-device', 'e1000-82545em,netdev=net1',
    '-rtc', 'base=utc,clock=host',
    '-serial', "file:$VmDirectory\serial.log",
    '-qmp', 'tcp:127.0.0.1:4444,server=on,wait=off',
    '-audiodev', 'sdl,id=audio0',
    '-device', 'thunderstorm,id=tsc0,present=on,version=0x011a0012,input=bars,stamp=host-ns,tstamp=host-s,timecode=utc,audio=silence,program-display=on,program-audio=on,audiodev=audio0',
    '-device', 'is1gl,id=is1gl0,mmio=0xfed10000,iobase=0x520',
    '-device', 'is1-clock',
    '-display', 'sdl,gl=off'
)

Push-Location $VmDirectory
try {
    & $Qemu @QemuArgs
} finally {
    Pop-Location
}
```
The VM runs under **TCG** (software emulation), as on macOS. Through some testing, it *should* be able to run with `-accel` set to **WHPX** (Windows Hypervisor Platform). However, not all hardware seems to work as FreeBSD instantly crash with a "Fatal trap" or similar error. If this happens on your end, leave `-accel` set to TCG.

QMP listens on **`127.0.0.1:4444`**.
</details>

<details>
<summary><b>macOS</b></summary>

Change `qemu-system-i386` to its complete path if you did not add the **qemu-is1/build** folder to your system's PATH. Compared to the Debian script, this one uses **TCG** instead of KVM (macOS has no KVM, and HVF doesn't support the i386 target), **CoreAudio** for sound, and the native **Cocoa** window.
```bash
cat > ~/i1/run.sh <<'EOF'
#!/bin/sh
IMG="$HOME/i1/weatherscan.img"
FORMAT=raw

# The logs, QMP socket and FIFOs live next to this script.
cd "$(dirname "$0")" || exit 1
exec qemu-system-i386 \
  -name IS1 \
  -machine pc,acpi=off \
  -global i440FX.agp=on -global i440FX.agp-aperture-size=128M \
  -global piix3-ide.force-bus-master=on \
  -accel tcg -cpu pentium3 -m 512 -smp 1 \
  -drive "file=$IMG,format=$FORMAT,if=ide,cache=writeback" -boot c \
  -vga cirrus \
  -netdev user,id=net0,net=10.100.102.0/24,host=10.100.102.1 \
  -device i82557b,netdev=net0 \
  -netdev user,id=net1,net=10.0.2.0/24,host=10.0.2.2,hostfwd=tcp:127.0.0.1:2222-10.0.2.15:22 \
  -device e1000-82545em,netdev=net1 \
  -rtc base=utc,clock=host \
  -serial file:./serial.log \
  -qmp unix:./qmp.sock,server,nowait \
  -audiodev coreaudio,id=audio0 \
  -device thunderstorm,id=tsc0,present=on,version=0x011a0012,\
input=bars,input-pipe=./is1-in-v,input-audio=./is1-in-a,\
stamp=host-ns,tstamp=host-s,timecode=utc,audio=silence,output=./is1-output,\
audiodev=audio0 \
  -device is1gl,id=is1gl0,mmio=0xfed10000,iobase=0x520 \
  -device is1-clock \
  -display cocoa,zoom-to-fit=on
EOF
chmod +x ~/i1/run.sh
```
To use GTK instead of Cocoa (if you built it), replace the last line with `-display gtk,zoom-to-fit=on,gl=off`. If you get no sound, try `-audiodev sdl,id=audio0` instead of `coreaudio`.
</details>

# Step 4: Creating the SSH scripts
The IS1's SSH server is old, so connecting to it needs a handful of legacy options. We'll make two small scripts so you never have to type them: one to log in, and one to copy the guest files from **virtsc** onto the VM. SSH is forwarded to the VM through **`127.0.0.1:2222`**.

Before saving the copy script, change the **virtsc** path in it to the folder where you cloned **virtsc**.

<details>
<summary><b>Debian/macOS/WSL</b></summary>


If you get the error "**Bad server host key: Invalid key length failure**", add ```-o RSAMinSize=1024``` to the ssh command

The SSH script:
```bash
cat > ~/i1/ssh.sh <<'EOF'
#!/bin/sh
exec ssh -p 2222 \
    -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o Ciphers=+aes128-cbc,3des-cbc \
    -o MACs=+hmac-sha1 \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    root@127.0.0.1
EOF
chmod +x ~/i1/ssh.sh
```
The copy script:
```bash
cat > ~/i1/copy-guest.sh <<'EOF'
#!/bin/sh
VIRTSC="$HOME/virtsc"

cd "$VIRTSC" || exit 1
exec scp -P 2222 \
    -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o Ciphers=+aes128-cbc,3des-cbc \
    -o MACs=+hmac-sha1 \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    src/agp/libagpnv.c \
    src/glfix/libglfix.c \
    src/is1gl/is1gl.c \
    src/is1gl/is1gl_ring.h \
    src/is1gl/is1gl_ops.h \
    src/is1gl/is1gl_gen_guest.h \
    src/is1clock/is1clock.c \
    src/is1clock/000.is1clock.sh \
    resources/XF86Config-4.qemu-cirrus \
    root@127.0.0.1:/usr/local/src/
EOF
chmod +x ~/i1/copy-guest.sh
```
</details>

<details>
<summary><b>Windows</b></summary>

Make sure **`ssh.exe`** and **`scp.exe`** are available. If they are missing, install the **Windows OpenSSH Client** optional feature before continuing.

Save the following as **`C:\IS1\SSH-VirTSC.ps1`**:
```powershell
ssh.exe -p 2222 `
  -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 `
  -o HostKeyAlgorithms=+ssh-rsa `
  -o PubkeyAcceptedAlgorithms=+ssh-rsa `
  -o Ciphers=+aes128-cbc,3des-cbc `
  -o MACs=+hmac-sha1 `
  -o StrictHostKeyChecking=no `
  -o UserKnownHostsFile=NUL `
  root@127.0.0.1
```
Save the following as **`C:\IS1\Copy-Guest.ps1`**:
```powershell
$Virtsc = 'C:\Users\i1\virtsc'
$GuestFiles = @(
    "$Virtsc\src\agp\libagpnv.c",
    "$Virtsc\src\glfix\libglfix.c",
    "$Virtsc\src\is1gl\is1gl.c",
    "$Virtsc\src\is1gl\is1gl_ring.h",
    "$Virtsc\src\is1gl\is1gl_ops.h",
    "$Virtsc\src\is1gl\is1gl_gen_guest.h",
    "$Virtsc\src\is1clock\is1clock.c",
    "$Virtsc\src\is1clock\000.is1clock.sh",
    "$Virtsc\resources\XF86Config-4.qemu-cirrus"
)

$SshOptions = @(
    '-P', '2222',
    '-o', 'KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1',
    '-o', 'HostKeyAlgorithms=+ssh-rsa',
    '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa',
    '-o', 'Ciphers=+aes128-cbc,3des-cbc',
    '-o', 'MACs=+hmac-sha1',
    '-o', 'StrictHostKeyChecking=no',
    '-o', 'UserKnownHostsFile=NUL'
)

& scp.exe @SshOptions @GuestFiles 'root@127.0.0.1:/usr/local/src/'
```
</details>

You won't be able to use these until the guest is set up for SSH, which is the next guide.

# Step 5: Starting the VM
Time to start the IS1! Be ready though, because on the first boot we need to interrupt the boot sequence to make some modifications.

<details>
<summary><b>Debian/WSL</b></summary>

```bash
~/i1/run.sh
```
</details>

<details>
<summary><b>Windows</b></summary>

Every script in `C:\IS1` is run the same way, with `powershell.exe -NoProfile -ExecutionPolicy Bypass -File`:
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File C:\IS1\Run-VirTSC.ps1
```
</details>

<details>
<summary><b>macOS</b></summary>

```bash
~/i1/run.sh
```
macOS may ask for permission to use the microphone or to accept incoming network connections. QEMU doesn't need either, so you can deny both. The port forward listens only on `127.0.0.1`.

Booting is slower than on Linux, since this is TCG emulation with no hardware acceleration for i386 on macOS. It should still run fine once it's up.

> **Mac keyboard note:** QEMU's **Ctrl+Alt** shortcuts are **Control+Option** on a Mac keyboard. Click in the QEMU window to capture the keyboard and mouse, and press **Control+Option+G** to release them.
</details>

Then head straight to the guest setup.

### Next: [Setting up the guest](GUEST.md)
