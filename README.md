# Kernel driver for QMK/VIA keyboards

## Introduction

This is a kernel driver meant to be used with keyboards that use QMK and have VIA enabled, and have a RGB backlight/matrix. This kernel module can expose functionality from your keyboard as sysfs entries, which can then be accessed by userspace applications. For now, the only functionality that is supported/exposed is the RGB matrix colors, exposed as a multicolor LED:

```
$ ls /sys/class/leds/ZSA\ Technology\ Labs\ Moonlander\ Mark\ I\:backlight/
brightness  device  max_brightness  multi_index  multi_intensity  power  subsystem  trigger  uevent
$ cat /sys/class/leds/ZSA\ Technology\ Labs\ Moonlander\ Mark\ I\:backlight/multi_intensity
155 96 185
```

My use case for this is to integrate it with KDE Plasma 6, which can [sync the keyboard's color with the active accent color on your desktop](https://pointieststick.com/2024/04/05/this-week-in-kde-real-modifier-only-shortcuts-and-cropping-in-spectacle/). Other use cases are entirely possible.

## Requirements

* Keyboard that:
  * uses QMK
  * has VIA enabled (or at least a VIA-like protocol enabled)
  * has RGB keys
* Linux kernel v5.9 or newer, with `CONFIG_LEDS_CLASS_MULTICOLOR` enabled (either as a module or built-in)

## Limitations

* Only one device is supported for now
* Devices plugged in after the kernel module is loaded aren't handled
* Other types of RGB backlights aren't supported
* Support other VIA versions

## WARNING

This kernel module has bugs, and there are obvious issues. Some of these may result in unreferenced memory accesses or memory leaks, depending on what you do when this module is used. You may have programs that mysteriously crash. You may have kernel panics.
