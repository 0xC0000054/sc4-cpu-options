# sc4-cpu-options

A DLL Plugin for SimCity 4 that configures the CPU core count and priority.

This plugin combines the functionality of the [SingleCPU](https://github.com/0xC0000054/sc4-single-cpu)
and [CPUPriority](https://github.com/0xC0000054/sc4-cpu-priority) plugins in a single DLL, and adds the ability to
set the CPU priority without using a command line argument.

If the `-CPUCount` and/or `-CPUPriority` command line arguments are present, those values will be used
in place of the plugin's default options. When those command line arguments are not present, the plugin
will configure SC4 to use 1 CPU core and the CPU priority specified in the configuration file.

This plugin is incompatible with 3rd-party launchers that set a CPU priority, it will override the CPU priority
setting they set when starting the game.
 
The plugin can be downloaded from the Releases tab: https://github.com/0xC0000054/sc4-cpu-options/releases

## System Requirements

* Windows 10 or later

The plugin may work on Windows 7 or later with the [Microsoft Visual C++ 2022 x86 Redistribute](https://aka.ms/vs/17/release/vc_redist.x86.exe)
installed, but I do not have the ability to test that.

## Installation

1. Close SimCity 4.
2. Remove `SC4SingleCPU.dll` and `SC4CPUPriority.dll` from the Plugins folder in the SimCity 4 installation directory, if present.
3. Copy `SC4CPUOptions.dll` and `SC4CPUOptions.ini` into the Plugins folder in the SimCity 4 installation directory.
4. Configure the plugin settings, see the *Configuring the plugin* section.

## Configuring the plugin

1. Open `SC4CPUOptions.ini` in a text editor (e.g. Notepad).
Note that depending on the permissions of your SimCity 4 installation directory you may need to start the text editor with administrator permissions to be able to save the file.

2. Adjust the settings in the `[CPUOptions]` section to your preferences.

3. Save the file and start the game.

### Settings overview

**Priority** is the CPU priority that the game will use, the default is *AboveNormal*.
The supported values are listed in the following table:

| Priority Value | Notes |
--------|--------
| High ||
| AboveNormal ||
| Normal | The default value for a process, unless the parent process has a different value. |
| BelowNormal ||
| Idle ||
| Low | An alias for Idle. |

## Troubleshooting

The plugin should write a `SC4CPUOptions.log` file in the same folder as the plugin.    
The log contains status information for the most recent run of the plugin.

# License

This project is licensed under the terms of the MIT License.    
See [LICENSE.txt](LICENSE.txt) for more information.

## 3rd party code

[gzcom-dll](https://github.com/nsgomez/gzcom-dll/tree/master) Located in the vendor folder, MIT License.    
[Windows Implementation Library](https://github.com/microsoft/wil) - MIT License    
[Boost.Algorithm](https://www.boost.org/doc/libs/1_84_0/libs/algorithm/doc/html/index.html) - Boost Software License, Version 1.0.    
[Boost.PropertyTree](https://www.boost.org/doc/libs/1_84_0/doc/html/property_tree.html) - Boost Software License, Version 1.0.

# Source Code

## Prerequisites

* Visual Studio 2022

## Building the plugin

* Open the solution in the `src` folder
* Update the post build events to copy the build output to you SimCity 4 application plugins folder.
* Build the solution

## Debugging the plugin

Visual Studio can be configured to launch SimCity 4 on the Debugging page of the project properties.
I configured the debugger to launch the game in a window with the following command line:    
`-intro:off -CPUCount:1 -CPUPriority:high -w -CustomResolution:enabled -r1920x1080x32`

You may need to adjust the resolution for your screen.
