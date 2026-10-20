# Building VirTSC on Arch-based Linux

This guide provides a walkthrough on building VirTSC on Arch-based Linux distrubtions
(i.e. Manjaro, EndeavourOS, CachyOS). If you are not using a Arch-based Linux, do not follow
this guide.

# Step 0: Using an AUR helper?

This guide instructs you on how to manually build VirTSC using PKGBUILD and .SRCINFO.
However, the AUR have disabled registrations for the foreseeable future, so we cannot
publish it for use with AUR helpers (i.e. `yay` or `paru`) at this time.

TLDR: You can still automate the build process pretty much entirely by using Arch's
official packaging tools in the meantime.

# Step 1: Prerequisites

Before you start, you must ensure your system has the core development tools and `git`
installed. Open your terminal and run:

```bash
sudo pacman -Syu base-devel git
```

- `base-devel` is a meta-package containing essential compilation tools like `gcc`, `make`,
  and `binutils`.
- `git` is required to download the repository which houses the `PKGBUILD` script.

You'll then need to clone the AUR repository. Keep it somewhere safe, it's how you'll update
VirTSC should that be required in the future.

This will (hopefully) be updated whenever we can publish to the AUR Git itself.

```bash
cd /directory/of/your/choice/
git clone https://github.com/VirTSC/virtsc-aur.git
cd virtsc-aur
```

# Step 2: Configuring & Building

While we're not looking to put 40 government cheese RATs on your machine, it's always good
practice to review any script you're executing.

```bash
cat PKGBUILD
```

You're now at the point where you can build and install VirTSC. This may take upwards of
10 minutes. You'll need the `makepkg` command for this:

```bash
makepkg -si
```

- `-s` stands for `--syncdeps`. It automatically downloads and installs missing software
  dependencies using `pacman` before attempting the build. Unless you want to hunt down all
  of the required dependencies by hand, we'd recommend this.
- `-i` stands for `--install`. It automatically installs the final binaries using `pacman`
  once the compilation finishes successfully.

Do **not** run `makepkg` as root or using `sudo`. The command itself will prompt you for
your root password when required.

### Next: [Running the VM](../RUN.md)
