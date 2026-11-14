UE4SS runs Lua mods inside the game, but Lua alone cannot reach everything a mod may need. Lua cannot ask Unreal's input system for native Tap and Hold triggers, cannot tell whether an object it still holds has been freed, has no notion of which files a mod should be allowed to touch, and cannot play a sound the game doesn't ship. UE4SS Lua Extensions are native C++ mods that add these abilities to UE4SS and expose them to other Lua mods through small Lua APIs.

There are three independent extensions. Each has its own mod folder, version and documentation. A mod depends on whichever it uses, and you can keep all of them installed. They do nothing on their own: install them when a mod asks for them.

[size=4][b]UE4SS Lua Event Bridge[/b][/size]
Connects Unreal Enhanced Input to UE4SS Lua callbacks. Bind existing Input Actions or create key bindings with native Tap and Hold triggers: Unreal handles the stopwatch, your mod handles the action. Lua receives copied event values outside Unreal's native input-dispatch stack.

It also provides native object-lifetime validation and weak handles, so a mod can tell that an object has been freed instead of reading it, plus session-owned handles, explicit cleanup, binding inspection, dispatch statistics and configurable queue budgets.

Mod folder: [font=Courier New]0_ModCore_UE4SSLuaEventBridge[/font]. Documentation: [url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-events/README.md]Event Bridge README[/url].

[size=4][b]UE4SS Lua File Bridge[/b][/size]
Gives Lua mods file and folder access inside the game's own folders. Each mod gets named locations (its own folder, a per-mod data folder, a scratch folder and the save folder), explicit write grants, atomic writes and file sockets that let mods talk through a path. Lua already has [font=Courier New]io[/font]; this is [font=Courier New]io[/font] with a seatbelt. The sandbox prevents mistakes; it is not a security boundary.

Mod folder: [font=Courier New]0_ModCore_UE4SSLuaFileBridge[/font]. Documentation: [url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-files/README.md]File Bridge README[/url].

[size=4][b]UE4SS Lua Sound Bridge[/b][/size]
Lets Lua mods play their own WAV files, outside the game's audio engine. A mod loads sounds from its own folder once, then plays them at once with pan, volume, pitch and a low-pass filter chosen per play; plays overlap, and playback follows the default output device when headphones are plugged in or out. Built for cues that must land with what is on screen, such as a sound that tells which side an attack comes from.

Mod folder: [font=Courier New]0_ModCore_UE4SSLuaSoundBridge[/font]. Documentation: [url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-sounds/README.md]Sound Bridge README[/url].

[size=4][b]Installation[/b][/size]
The Nexus download is one ZIP holding all three extensions. Extract it into the UE4SS [font=Courier New]Mods[/font] directory, or install it with your mod manager. It adds a folder for each extension and a third, Lua-only folder for the menu:

[code]Mods/
├── 0_ModCore_UE4SSLuaEXtensions/
├── 0_ModCore_UE4SSLuaEventBridge/
├── 0_ModCore_UE4SSLuaFileBridge/
└── 0_ModCore_UE4SSLuaSoundBridge/[/code]

[font=Courier New]0_ModCore_UE4SSLuaEXtensions[/font] lists the installed extensions in ModCore Settings' mod list as one module, ModCore Lua Extensions. Its page shows how many extensions are installed, then a section for each one holding that extension's own settings (from its mod_settings.ini). The extensions are then not listed as separate mods. Without ModCore Settings, or without this folder, the extensions work the same and are listed on their own.

Each extension folder's [font=Courier New]docs[/font] subfolder holds the README, developer guide, examples and license for mod authors.

Each extension ships an [font=Courier New]enabled.txt[/font] marker, so it loads without any [font=Courier New]mods.txt[/font] entry. The extensions are native DLL mods, which UE4SS loads before Lua mods, and their [font=Courier New]0_[/font] folder names sort ahead of the mods that use them. Keep only one active installation of each extension. Each extension is also released separately on [url=https://github.com/jrpereira/UE4SSLuaEXtensions/releases]GitHub[/url].

[size=4][b]Compatibility[/b][/size]
Windows x64 (the Sound Bridge needs Windows 10 or later), Unreal Engine 5.5, and UE4SS 3.0.1 Beta #0 at commit [font=Courier New]97b7e501[/font]. Native layouts are pinned to that build; nearby engine or UE4SS builds are not assumed compatible. Check [font=Courier New]GetCapabilities()[/font] in each extension's Lua API for available features rather than inferring them from a version number.

[size=4][b]Documentation[/b][/size]
[list]
[*][url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-events/README.md]Event Bridge README[/url] and [url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-events/docs/DEVELOPERS.md]developer guide[/url]
[*][url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-files/README.md]File Bridge README[/url], [url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-files/docs/DEVELOPERS.md]developer guide[/url] and [url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-files/docs/API.md]API reference[/url]
[*][url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-sounds/README.md]Sound Bridge README[/url] and [url=https://github.com/jrpereira/UE4SSLuaEXtensions/blob/main/bridge-sounds/docs/API.md]API reference[/url]
[*]Source, issues and releases: [url=https://github.com/jrpereira/UE4SSLuaEXtensions]GitHub[/url]
[/list]
