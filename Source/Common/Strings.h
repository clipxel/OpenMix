#pragma once

#include <juce_core/juce_core.h>

namespace om
{

/** UTF-8 string literal: "Configuración"_u. JUCE treats plain char* as ASCII. */
inline juce::String operator""_u (const char* s, size_t n) { return juce::String::fromUTF8 (s, (int) n); }

} // namespace om
