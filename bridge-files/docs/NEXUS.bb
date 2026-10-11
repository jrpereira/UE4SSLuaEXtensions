[size=5][b]UE4SS Lua File Bridge[/b][/size]

A library for mod authors. It gives UE4SS Lua mods file and folder access inside the game's own folders, with safe defaults. It does nothing on its own: install it when a mod asks for it.

[size=4][b]For players[/b][/size]
[list]
[*]Install with your mod manager, or extract the ZIP into [font=Courier New]ue4ss/Mods/[/font].
[*]Requires UE4SS 3.0.1 Beta #0 (commit 97b7e501). Other UE4SS builds are not supported.
[*]Optional: list it in [font=Courier New]Mods/mods.txt[/font] before the mods that use it:
[code]0_ModCore_UE4SSLuaFileBridge : 1[/code]
[/list]

[size=4][b]For mod authors[/b][/size]
[list]
[*][b]Named locations:[/b] your mod's folder, a per-mod data folder that survives updates, a scratch folder emptied at start, and the save folder.
[*][b]Safe by default:[/b] read-only outside your own folder; writing elsewhere is granted per path, mode and file extension.
[*][b]Atomic writes[/b], and save files keep one [font=Courier New].bak[/font] on every overwrite.
[*][b]Append-only logs[/b] and write streams others can read while open.
[*][b]File sockets:[/b] one mod writes a file, another is called back with each new line.
[/list]

[code]local files = UE4SSLuaFileBridge()
files.WriteText("mod/state.json", '{"runs":1}')
print(files.ReadText("mod/state.json"))[/code]

The bridge prevents mistakes; it isn't a security boundary. Lua mods can still use the standard library directly.

Documentation, examples and source: [url=https://github.com/jrpereira/UE4SSLuaExtensionBridges]GitHub[/url] (folder [font=Courier New]bridge-files/[/font]).

[size=4][b]Compatibility[/b][/size]
Windows x64, Unreal Engine 5.5, UE4SS 3.0.1 Beta #0 at 97b7e501. Works alongside UE4SS Lua Event Bridge.
