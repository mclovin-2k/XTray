#pragma once

#include <windows.h>

namespace xtray {

enum class Language { Automatic, English, Chinese };

Language ResolveLanguage(Language preference, LANGID displayLanguage);
void ApplyLanguage(Language preference);
Language CurrentLanguage();
const wchar_t* Text(const wchar_t* english, const wchar_t* chinese);
const char* Text(const char* english, const char* chinese);

} // namespace xtray
