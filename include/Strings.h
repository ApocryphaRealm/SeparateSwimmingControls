#pragma once

// Player-visible text in the eleven shipped languages (rule 66).
//
// TR("PREFIX_Key", "English text") returns the text for the game's language, read from
//     Data\Interface\Translations\SeparateSwimmingControls_<language>.txt
// (UTF-16LE with BOM, one "$key<TAB>text" per line - the SKSE/SkyUI shape). A key the language
// file lacks falls back to the English file, then to the compiled English, so a half-finished
// translation never shows a raw key. Text only a tool or the log reads stays English.
namespace ssc::Strings
{
    // Reads the language files once. Call at kDataLoaded, when the game's INI settings are final.
    void Load();

    // Text for a key; english is the compiled fallback.
    std::string Get(std::string_view key, std::string_view english);

    // The language that was loaded ("english" until Load has run), for the log and the DevBench tool.
    std::string Language();
}

#define TR(key, english) ::ssc::Strings::Get(key, english)
