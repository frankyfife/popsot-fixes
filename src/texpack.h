#pragma once

// Texture packs made with PoP Texture Studio (d3d9.dll + Evgesha.JK in the
// game folder), see texpack.cpp. TexPack_Load runs in DllMain, before the game
// opens prince.bf; TexPack_HookSystem before our own system vtable hooks.
void TexPack_Load(const char* gameDir, const char* file, bool keepBloom, const char* skip);
void TexPack_HookSystem();
