#include "I18n.h"

#include <array>
#include <atomic>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace OST::ExtractTickets {

namespace {

#define OST_I18N_TABLE_DEF(key, en, zh, es) std::array<std::string_view, kLanguageCount>{ en, zh, es },

constexpr std::array<std::string_view, kLanguageCount> kTranslations[] = {
    OST_I18N_MESSAGES(OST_I18N_TABLE_DEF)
};

#undef OST_I18N_TABLE_DEF

static_assert(std::size(kTranslations) == kMsgKeyCount, "kTranslations size must match kMsgKeyCount exactly!");

std::atomic<Language> s_currentLang{Language::English};

} // namespace

Language I18n::DetectSystemLanguage() noexcept {
    char envBuf[64]{0};
#if defined(_WIN32)
    DWORD envLen = GetEnvironmentVariableA("OST_LANG", envBuf, sizeof(envBuf));
    if (envLen > 0 && envLen < sizeof(envBuf)) {
        std::string_view envVal(envBuf);
        if (envVal.starts_with("zh") || envVal.starts_with("ZH") || envVal == "chinese" || envVal == "schinese") {
            return Language::Chinese;
        }
        if (envVal.starts_with("es") || envVal.starts_with("ES") || envVal == "spanish") {
            return Language::Spanish;
        }
        if (envVal.starts_with("en") || envVal.starts_with("EN") || envVal == "english") {
            return Language::English;
        }
    }

    LANGID langId = GetUserDefaultUILanguage();
    WORD primaryLang = PRIMARYLANGID(langId);
    if (primaryLang == LANG_CHINESE) {
        return Language::Chinese;
    }
    if (primaryLang == LANG_SPANISH) {
        return Language::Spanish;
    }
#endif
    return Language::English;
}

Language I18n::GetCurrentLanguage() noexcept {
    return s_currentLang.load(std::memory_order_relaxed);
}

void I18n::SetLanguage(Language lang) noexcept {
    s_currentLang.store(lang, std::memory_order_relaxed);
#if defined(_WIN32)
    SetEnvironmentVariableA("OST_LANG", GetLanguageCode(lang).data());
#endif
}

void I18n::InitLanguage() noexcept {
    SetLanguage(DetectSystemLanguage());
}

std::string_view I18n::GetLanguageCode(Language lang) noexcept {
    switch (lang) {
        case Language::Chinese: return "zh";
        case Language::Spanish: return "es";
        case Language::English:
        default:                return "en";
    }
}

const char* I18n::GetSteamLanguageCode(Language lang) noexcept {
    switch (lang) {
        case Language::Chinese: return "schinese";
        case Language::Spanish: return "spanish";
        case Language::English:
        default:                return "english";
    }
}

std::string_view I18n::Get(MsgKey key) noexcept {
    const size_t kIdx = static_cast<size_t>(key);
    if (kIdx >= kMsgKeyCount) return "";
    const size_t lIdx = static_cast<size_t>(s_currentLang.load(std::memory_order_relaxed));
    return kTranslations[kIdx][lIdx < kLanguageCount ? lIdx : 0];
}

} // namespace OST::ExtractTickets
