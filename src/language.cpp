#include "language.h"

#include <atomic>

namespace xtray {
namespace {

std::atomic<Language> current{Language::English};

} // namespace

Language ResolveLanguage(Language preference, LANGID displayLanguage)
{
    if (preference == Language::English || preference == Language::Chinese) {
        return preference;
    }
    return PRIMARYLANGID(displayLanguage) == LANG_CHINESE ? Language::Chinese : Language::English;
}

void ApplyLanguage(Language preference)
{
    current.store(ResolveLanguage(preference, GetUserDefaultUILanguage()));
}

Language CurrentLanguage()
{
    return current.load();
}

const wchar_t* Text(const wchar_t* english, const wchar_t* chinese)
{
    return CurrentLanguage() == Language::Chinese ? chinese : english;
}

const char* Text(const char* english, const char* chinese)
{
    return CurrentLanguage() == Language::Chinese ? chinese : english;
}

} // namespace xtray
