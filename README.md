# Pacman Mirror

> [!NOTE]
> This is the C implementation of the original [pacmirror.sh](https://codeberg.org/aocoronel/pacmirror.sh). See the peculiarities of this edition in [`C Edition`](#c-edition).

`pacmirror` is a tool that allows you to declare what packages are installed in your system. It's highly inspired in [NixOS](https://nixos.org/) and [rebos](https://gitlab.com/oglo12/rebos), but it's main goals are to simply allow the user to declare the packages they want to install in their system using `pacman`.

To use this, you must read the [`Setup`](#setup) section to properly understand how to use it. Use with caution!

How it works? `pacmirror` will extract all your official packages and AUR packages and compare with the packages you provide it, so it can remove all the packages not in your provided list, and install the ones you haven't provided. You will mostly want to add all your system packages first, so the it does not try to remove important programs from your system such as `grub`, `base` and `base-devel`. So, you decide exactly what programs to be installed. If your configuration does not have `grub`, so it shouldn't be installed.

Differently than [rebos](https://gitlab.com/oglo12/rebos) there is no setup step, which would safely register all your currently installed programs, so you can calmly build your rebos configuration file.

## Features

- Install and remove pacman packages
- Install and remove AUR packages
- Organize your config using C

## C Edition

Differently from the POSIX Shell implementation of `pacmirror`, this edition focuses on allowing the user to configure using the C programming language, primarily, but it should be possible to interop with another programming language.

`pacmirror` gives you full control over how you control your configuration, so you can build the required packages at compile time or runtime

Currently, `pacmirror` doesn't try to replace `pacman` neither any existing AUR helper, because of this it will run a subprocess of `pacman` to install official packages and uninstall packages, and `makepkg` to install local PKGBUILDs.

## Building

If you plan to use configured in C, the building process is rather simple, being just a single header file.

```bash
# .
# ├── pacmirror.c # User provided which includes pacmirror.h
# └── pacmirror.h

gcc ./pacmirror.c -lalpm -o pacmirror
```

## Usage

```bash
pacmirror
pacmirror -s sudo # sudo, doas
```

#### Setup

To get started, you may add all your installed packages to a starting configuration file. You must provide all your system packages that were **explicitly installed** by the user, do not include dependencies, unless you need it.

```bash
# To get all your native packages (official packages)
pacman -Qqen >> "pacmirror.c"
# AUR packages
pacman -Qqem >> "pacmirror.c"
```

After that, you can construct your array as a compile-time known, or at runtime.

```c
char *pacman[] = {
  "bash",
  "zsh",
  ...
  NULL, // Remember the NULL termination
}

char *aur[] = {
  "tomb",
  "lesspass",
  ...
  NULL,
}
```

To run `pacmirror` you have to call the `int pacmirror(char **pacman, char **aur, int argc, char **argv)` function. This function requires you to provide two arrays with packages, which requires **NULL termination**, and just forward `argc` and `argv` from main. For instance, you can keep it tidy:

```c
int main(int argc, char **argv) {
    return pacmirror(pacman, aur, argc, argv);
}
```

You may also take advantages of some functions form `pacmirror.h` like `void init_da()` to initialize a dynamic array and build your array with it using `da_append()` and `da_append_null()`.

If you are an Artix Linux user, and you use gremlins packages, you may also compile with the `ARTIX_GREMLINS` define to enable those repositories.

If you want to use AUR packages, there is quite a couple of things you should do. Previously `pacmirror` was using an AUR helper of your liking, however `pacmirror` will now look into `./pkg/`, where it contains other directories with the exact same name of the AUR packages, and within them should be their `PKGBUILD`, you may copy paste them from the AUR, or maintain it yourself. For example, say you want to install `freetube-bin`: go to the AUR repository, and go to the PKGBUILD. Review it and copy it to `./pkg/freetube-bin/PKGBUILD`. The `aur[]` array should contain `freetube-bin` as well.
```

## FAQ

### warning: x is up to date -- skipping

If each time you run `pacmirror` and pacman complains that there are packages already up-to-date, it means these packages, where installed as dependencies to another package. You can fix it, by reinstalling them explicitly as follows: `pacman -S --asexplicit`.

## License

This repository is licensed through the GNU General Public License, version 2 or later.
