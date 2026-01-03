**[NOTE]** This reposity is mirrored from [Codeberg](https://codeberg.org/ponleou/pipetron).

---

# Pipetron

Pipetron (**Pipe**Wire + Elec**tron**, _very creative_) is a bandage fix for Electron app audio streams with PipeWire. Electron apps have a long-running issue with being unable to change its audio stream names from the permanent "Chromium" name. Though minor, it creates annoyances such as being unable to differentiate Electron apps within volume controller apps that uses PipeWire's `application.name` (a popular example includes pavucontrol), WirePlumber resetting all Electron apps to the same audio setting, and perhaps more. Pipetron aims to fix the former two.

Pipetron's solution is simple: replicate all Electron audio streams with it's actual Electron app name and icons, and make a one-way sync of its volume settings from the replicated stream to its corresponding Electron stream. As a byproduct, WirePlumber saves the replicated streams volume settings, and Pipetron syncs the volume setting to its Electron stream.

## Images

Without Pipetron:

<img src="./img/pavucontrol_without_pipetron.png" alt="pavucontrol without Pipetron" width=45% />

With Pipetron:

<img src="./img/pavucontrol_with_pipetron.png" alt="pavucontrol with Pipetron" width=45% />

## Installing

### Arch Linux

Pipetron is available in the [AUR](https://aur.archlinux.org/packages/pipetron) (maintained by [me](https://codeberg.org/ponleou)).

```
yay -S pipetron
```

By default, Pipetron's systemd service `pipetron.service` will be enabled and will start in the next session. To start it immediately:

```
systemctl --user start pipetron.service
```

### Manual build

**[DISCLAIMER]** this project is very young, and is currently untested within other distros besides Arch Linux. Feel free to create [issues](https://codeberg.org/ponleou/pipetron/issues) or [PRs](https://codeberg.org/ponleou/pipetron/pulls) on package maintainers.

#### Requirements

-   `meson`
-   `pipewire` and `libpipewire`

```
git clone https://codeberg.org/ponleou/pipetron.git
cd pipetron
meson setup build
meson compile -C build
sudo meson install -C build
```

Start and enable the systemd service by:

```
systemctl --user enable --now pipetron.service
```
