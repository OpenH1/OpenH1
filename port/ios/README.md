# iOS and iPadOS

`ninja ios` builds the game for iPhones and iPads (arm64, iOS 17.4 or
later) on a Mac with Apple silicon and Xcode. `ninja ios_sim` builds it for
the iOS Simulator on that Mac.

| Result | Item |
| --- | --- |
| `build/ios/OpenH1.app` | The app, signed for development with `configure.py --ios-team`, else ad hoc |
| `build/ios/halo` | Its executable, with SDL 3.4.16 linked in (without OpenGL) |
| `build/ios_sim/OpenH1.app` | The Simulator's app (arm64), signed ad hoc |

It is the macOS build (refer to [port/macos/README.md](../macos/README.md))
built for iOS: the same guest (the game as ILP32 AArch64 code, with its
memory at a base register) with the iOS code paths (`HALO_IOS`) beside the
Mac's (`HALO_MACOS`), and the same host (`port/android/host`,
`port/linux/host`, `port/macos`) with UIKit's app around it (`port/ios`).
The renderer is the Metal backend of the macOS build. It was tested on an
iPhone 15 Pro Max (A17 Pro) with iOS 27, and in the Simulator of iOS 26.5.

The App Store does not take the game, a decompilation of a commercial game.
The app is for sideloading: installed from a Mac with Xcode's tools (below),
or signed again by a sideloading tool (AltStore, SideStore) from the ad hoc
build.

## Requirements

- A Mac with Apple silicon, and Xcode 15 or later with its iOS SDK (and its
  Simulator's, for `ninja ios_sim`). The iOS build is set up whenever
  `configure.py` finds the SDK.
- What the macOS build needs (refer to its README): the pinned LLVM for the
  guest, CMake for SDL3, a network connection for the first build
  (`configure.py` downloads musl and SDL3 to `build/ios/third_party`).
- For a device: an Apple developer team (a paid membership: the app's
  entitlements, below, are not given to free teams), signed in to Xcode
  (Xcode, Settings, Accounts), and the device in Developer Mode (Settings,
  Privacy & Security, Developer Mode).

## Build and install

```sh
python3 configure.py --ios-team <team ID>      # e.g. --ios-team 5NH5B2VG6Y
ninja ios                                       # build/ios/OpenH1.app
ninja ios_install                               # onto the connected iPhone or iPad
python3 tools/ios_app.py launch                 # start it
python3 tools/ios_app.py log                    # its log (Documents/logs/halo.log)
```

### Signing

With `--ios-team`, the first `ninja ios` makes a development provisioning
profile (`build/ios/embedded.mobileprovision`) with
`tools/ios_provision.py`: the app is built by ninja, not by Xcode, but only
Xcode makes profiles without an App Store Connect key, so the script writes
a stand-in Xcode project (one empty app with the app's bundle identifier and
entitlements) and builds it with `xcodebuild -allowProvisioningUpdates`.
That registers the bundle identifier (`com.openh1.openh1`, the macOS app's;
`--ios-bundle-id` chooses another), turns on the capabilities the
entitlements name, and makes a profile that holds the team's registered
devices (Xcode registers a connected device). Run it again after adding a
device: `python3 tools/ios_provision.py --team <team ID>`.

`tools/ios_app.py` signs the app with the team's Apple Development
certificate (`--ios-identity` names another), the profile and
`port/ios/halo.entitlements`:

- `com.apple.developer.kernel.extended-virtual-addressing`: the guest's
  4 GB are reserved at once, at a multiple of 4 GB, with guards
  (`host_memory_base.c`): more address space than an app has without it.
- `com.apple.developer.kernel.increased-memory-limit`: the game's memory
  is more than the default limit of some devices.

Without `--ios-team` the app is signed ad hoc, without a profile: a device
refuses it, but a sideloading tool signs it again with its own.

## The guest's code

iOS gives apps no memory that was writable and is executable: not with
`mprotect`, and not `MAP_JIT`'s (allowed only under a debugger). The other
builds' loader (`port/android/host/host_loader.c`) writes the guest image's
code into memory and makes it executable, so on iOS the code is part of the
signed executable instead, at the guest's addresses:

1. The guest image (the ELF file the other builds load) is in a section of
   the executable's `__TEXT`, `__TEXT,__halo_guest`, on a 16 KB boundary
   (`port/ios/guest_image.S`). The image's segments lie at file offsets
   that equal their addresses modulo 64 KB (ld.lld's alignment), and its
   header, code and constants (`r--`, `r-x`, `r--`) are one run of the
   file, below the data (`rw-`), which starts on a page of its own.
2. The loader reserves the guest's 4 GB as on macOS, at a base that is a
   multiple of 4 GB (`host_memory_base.c`), copies the image there and fills
   its import table (in the image's `.data`).
3. It then maps the read-only part (`0x88000000` up to the data) again,
   from the executable's file (`dladdr` gives its path and the image's
   offset in it), with `mmap(MAP_FIXED, PROT_READ | PROT_EXEC)`: the pages
   of the signed executable, which the kernel checks against the app's
   signature as it does any of the executable's. Nothing is written into
   code or made executable by the host.

The base register makes this possible: every guest memory access and
indirect branch is the base plus a 32-bit offset
(`tools/guest_base_register.py`), and the base's low 32 bits are zero, so
code mapped at base plus its link address works anywhere in the process,
and its PC-relative references to its data reach the data at base plus the
data's address. No relocation is needed: the image is static, linked at
its guest addresses, and its only fix-ups, the host functions it calls,
are the import table's entries, in the data.
`host_image_read_only_range` (in `host_loader.c`, tested by
`port/android/tests/host_loader_test.c`) checks that the image is laid out
so.

The Simulator is a Mac process, without iOS's code signing rules: the same
mapping runs there, but proves nothing of the device's.

## The app

`tools/ios_app.py` makes `OpenH1.app`:

| Item | What |
| --- | --- |
| `halo` | The executable |
| `Assets.car`, `AppIcon*.png` | The icon (actool's, from `port/ios/openh1-icon-1024.png`, which `tools/app_icon.py` draws) |
| `brokers.txt` | Internet play's brokers, copied beside `config.toml` at each start |
| `shaders.metallib` | The Metal shaders, made ahead of time (`tools/macos_shaders.py --platform ios`) |
| `Licenses/` | The licences of OpenH1 and of what the app carries |
| `embedded.mobileprovision` | The development profile (with `--ios-team`) |

`Info.plist`: the macOS app's identifier and version (the build's number),
landscape only, full screen without the status bar, `UILaunchScreen`
(black), UIKit's scenes (SDL's scene delegate), `UIFileSharingEnabled` and
`LSSupportsOpeningDocumentsInPlace` (the Files app shows the app's
Documents), `GCSupportsControllerUserInteraction`, and the microphone's
(voice chat) and the local network's (system link) explanations.

When it starts (`port/ios/host_app.m`), the host puts the game's files in
the app's Documents folder: `config.toml`, the data root (`maps`,
`custom_maps`, `debug.txt`), the saves (`saves`), and the host's log
(`logs/halo.log`, the previous start's `logs/halo.previous.log`). The Metal
pipelines' archive is in `Library/Caches/OpenH1`.

`main` hands the process to UIKit (`SDL_RunApp`), which calls the host back
once the app has launched; the game then runs on the main thread as on
macOS, and its event polls turn UIKit's run loop.

### The game data

For now, copy the Xbox game's `maps` folder into the app's Documents with
the Files app (On My iPhone, OpenH1) or Finder's device window (Files), or
from a Mac:

```sh
xcrun devicectl device copy to --device <device> --domain-type appDataContainer \
    --domain-identifier com.openh1.openh1 --source <folder>/maps --destination Documents/maps
```

### The background

As the app goes to the background, the presentation stops (iOS refuses GPU
work from the background) and the game waits in its next event poll until
the app is back; iOS suspends the process meanwhile. In a network game, the
other machines drop it after 15 seconds, as on Android.

## Metal on iOS

The Metal backend is the macOS one. What differs:

- iPhones' GPUs (and the Simulator's) have no BC texture formats: their
  DXT textures are decoded when uploaded (`xgpu_metal_block_compression`,
  `xgpu_metal_textures.c`), as the OpenGL ES backend decodes them for Mali.
  iPads with an M chip sample them as they are.
- The screen is a `UIScreen`: its refresh rate (ProMotion's 120 Hz), and
  its EDR headroom for `display.hdr`.
- The Simulator's GPU has no binary archives (the pipelines' archive is not
  used there) and its drawables say nothing of their presentation.

## The Simulator

```sh
xcrun simctl list devices                       # a device, e.g. an iPhone's
xcrun simctl boot <device>                      # headless; Simulator.app shows it if open
ninja ios_sim
python3 tools/ios_app.py install --simulator
python3 tools/ios_app.py launch --simulator
python3 tools/ios_app.py log --simulator
xcrun simctl io <device> screenshot shot.png
```

Its data folder is `xcrun simctl get_app_container <device> com.openh1.openh1 data`;
put `maps` in its `Documents`.

## Not done yet

- Touch controls (the Android app's are Java: `port/android/app`), and the
  first start's import of a disc image.
- The safe areas: the game draws over the whole screen, the sensor housing's
  and the home indicator's edges too.
- System link on a device: iOS lets apps send broadcasts only with the
  multicast networking entitlement, which Apple grants on request.
