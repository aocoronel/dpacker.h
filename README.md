# Pacman Mirror

This is the C implementation of the original [pacmirror.py](https://codeberg.org/aocoronel/pacmirror.py) in Bash + Python. This edition is written in C and configured using C.

`pacmirror` is a tool that allows you to declare what packages are installed in your system. It's highly inspired in [NixOS](https://nixos.org/) and [rebos](https://gitlab.com/oglo12/rebos), but it's main goals are to simply allow the user to declare the packages they want to install in their system using `pacman`.

To use this, you must read the [`Setup`](#setup) section to properly understand how to use it. Use with caution!

This script will use `pacman -Qqe` to compare the system packages to your configuration, which means only programs explicitly installed by the user, thus does not include dependencies. You will mostly want to add all your system packages first, so the script does not try to remove important programs from your system such as `grub`, `base` and `base-devel`.

Differently than [rebos](https://gitlab.com/oglo12/rebos) there is no setup step, which would safely register all your currently installed programs, so you can calmly build your rebos configuration file.

If you run this tool without a properly setting up configuration file it will prompt you to uninstall all everything from your machine. This is a design choice. `pacmirror` will refuse to operate without a config, and won't perform any AUR operations without a supported AUR Helper.

You decide exactly what programs to be installed: If your configuration does not have `grub`, so it shouldn't be installed.

## Features

- Install and remove pacman packages
- Install and remove AUR packages
- Organize your config using C

## Building

This program is a single file, but depends on a user provided `pacmirror.h`, unless you change the source code.

```bash
git clone https://github.com/aocoronel/pacmirror.c
cd pacmirror.c
gcc ./src/pacmirror.c -lalpm -o pacmirror
ln -s $(pwd)/pacmirror $HOME/.local/bin
```

## Usage

```bash
pacmirror
pacmirror -a yay # yay, paru, pikaur
pacmirror -s sudo # sudo, doas
```

### Configuration

**Currently there is support for:** Pacman packages and AUR packages.

The `pacmirror.h` needs to define two global variables, `pacman` and `aur`, as of type: `const char *pacman[]`.

#### Setup

To get started, you can add all your installed packages to a starting configuration file:

```bash
# Add all your system packages to a configuration file that were
# explicitly installed by the user

# Native packages
pacman -Qqen >> "pacmirror.h"
# AUR packages
pacman -Qqem >> "pacmirror.h"
```

After that, assign the packages into the required type, and it should be safe to start using `pacmirror` without the risk to compromise important programs.

Quick example:

```c
const char *pacman[] = {
  "bash",
  "zsh",
}

const char *aur[] = {
  "tomb",
  "lesspass",
}
```

## FAQ

### warning: x is up to date -- skipping

If each time you run `pacmirror` and pacman complains that there are packages already up-to-date, it means these packages, where installed as dependencies to another package. You can fix it, by reinstalling them explicitly as follows: `pacman -S --asexplicit`.

## License

This repository is licensed under the MIT License, allowing for extensive use, modification, copying, and distribution.
