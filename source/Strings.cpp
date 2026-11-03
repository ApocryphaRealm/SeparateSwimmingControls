#include "PCH.h"

#include "Strings.h"

#include <format>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace swimcontrols::Strings
{
    namespace
    {
        constexpr std::string_view kStem = "SwimControls";

        std::mutex lock;
        std::string language = "english";
        std::unordered_map<std::string, std::string> localized;  // the game's language
        std::unordered_map<std::string, std::string> english;    // fallback

        // UTF-16 to UTF-8 by hand, so this file needs no Windows header beside CommonLib's.
        std::string ToUtf8(const std::wstring& wide)
        {
            std::string out;
            out.reserve(wide.size());
            for (std::size_t i = 0; i < wide.size(); ++i) {
                std::uint32_t cp = static_cast<std::uint16_t>(wide[i]);
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wide.size()) {
                    const std::uint32_t low = static_cast<std::uint16_t>(wide[i + 1]);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        ++i;
                    }
                }
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }
            return out;
        }

        // Reads one translation file. Returns an empty map (and logs) when the file is missing or unreadable.
        std::unordered_map<std::string, std::string> ReadFile(std::string_view lang)
        {
            std::unordered_map<std::string, std::string> keys;
            const auto path = std::filesystem::absolute(std::format("Data\\Interface\\Translations\\{}_{}.txt", kStem, lang));

            std::ifstream file(path, std::ios::binary);
            if (!file) {
                SKSE::log::debug("No translation file at {}.", path.string());
                return keys;
            }

            const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (raw.size() < 2 || static_cast<unsigned char>(raw[0]) != 0xFF || static_cast<unsigned char>(raw[1]) != 0xFE) {
                SKSE::log::warn("Translation file {} is not UTF-16LE with a BOM; it was skipped.", path.string());
                return keys;
            }

            std::wstring text((raw.size() - 2) / 2, L'\0');
            std::memcpy(text.data(), raw.data() + 2, text.size() * sizeof(wchar_t));

            std::size_t start = 0;
            while (start < text.size()) {
                auto end = text.find(L'\n', start);
                if (end == std::wstring::npos) end = text.size();
                std::wstring line = text.substr(start, end - start);
                start = end + 1;

                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (line.size() < 2 || line[0] != L'$') continue;
                const auto tab = line.find(L'\t');
                if (tab == std::wstring::npos) continue;

                keys[ToUtf8(line.substr(1, tab - 1))] = ToUtf8(line.substr(tab + 1));
            }

            SKSE::log::debug("Read {} translation key(s) from {}.", keys.size(), path.string());
            return keys;
        }

        // The game's language from sLanguage:General. A missing setting (null) keeps English, which is always shipped.
        std::string GameLanguage()
        {
            auto* settings = RE::INISettingCollection::GetSingleton();
            auto* setting  = settings ? settings->GetSetting("sLanguage:General") : nullptr;
            if (!setting || setting->GetType() != RE::Setting::Type::kString || !setting->GetString()) {
                SKSE::log::warn("sLanguage:General is unavailable; using English text.");
                return "english";
            }

            std::string lang = setting->GetString();
            std::ranges::transform(lang, lang.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lang.empty() ? std::string("english") : lang;
        }
    }

    void Load()
    {
        auto lang     = GameLanguage();
        auto fallback = ReadFile("english");
        auto chosen   = lang == "english" ? fallback : ReadFile(lang);

        if (fallback.empty()) {
            SKSE::log::warn("The English translation file is missing; compiled English text will be used.");
        }

        std::scoped_lock guard(lock);
        language  = std::move(lang);
        english   = std::move(fallback);
        localized = std::move(chosen);
        SKSE::log::info("Text language: {} ({} localized key(s), {} English key(s)).", language, localized.size(), english.size());
    }

    std::string Get(std::string_view key, std::string_view englishText)
    {
        std::scoped_lock guard(lock);
        const std::string k(key);
        if (const auto it = localized.find(k); it != localized.end() && !it->second.empty()) return it->second;
        if (const auto it = english.find(k); it != english.end() && !it->second.empty()) return it->second;
        return std::string(englishText);
    }

    std::string Language()
    {
        std::scoped_lock guard(lock);
        return language;
    }
}
