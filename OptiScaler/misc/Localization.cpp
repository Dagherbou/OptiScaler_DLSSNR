#include "pch.h"
#include "Localization.h"

#include <Util.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace
{
struct StringHash
{
    using is_transparent = void;

    size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view> {}(value); }
};

struct Dictionary
{
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> translations;
    bool chinese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ||
                   PRIMARYLANGID(GetThreadUILanguage()) == LANG_CHINESE;
};

std::string_view Trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
        return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

std::string Unescape(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\\' && i + 1 < text.size())
        {
            switch (text[i + 1])
            {
            case 'n': result += '\n'; ++i; continue;
            case 'r': result += '\r'; ++i; continue;
            case 't': result += '\t'; ++i; continue;
            case 's': result += ' '; ++i; continue;
            case '\\': result += '\\'; ++i; continue;
            case '=': result += '='; ++i; continue;
            }
        }
        result += text[i];
    }
    return result;
}

bool IsUtf8(std::string_view text)
{
    return !text.empty() && text.size() <= static_cast<size_t>((std::numeric_limits<int>::max)()) &&
           text.find('\0') == std::string_view::npos &&
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}

bool IsDigit(char value)
{
    return value >= '0' && value <= '9';
}

// Strictly matching the complete directives also preserves star arguments, integer length,
// wide/narrow strings, and bounded widths/precisions. Positional parameters, %n, and unknown
// extensions are deliberately rejected rather than interpreted differently by the CRT/ImGui.
bool NextDirective(std::string_view text, size_t& cursor, std::string_view& directive)
{
    const auto begin = text.find('%', cursor);
    directive = {};
    if (begin == std::string_view::npos)
        return true;

    size_t end = begin + 1;
    if (end < text.size() && text[end] == '%')
    {
        cursor = end + 1;
        directive = text.substr(begin, cursor - begin);
        return true;
    }

    while (end < text.size() && std::string_view("-+ #0").find(text[end]) != std::string_view::npos)
        ++end;
    if (end < text.size() && text[end] == '*')
        ++end;
    else
        while (end < text.size() && IsDigit(text[end]))
            ++end;
    if (end < text.size() && text[end] == '.')
    {
        ++end;
        if (end < text.size() && text[end] == '*')
            ++end;
        else
            while (end < text.size() && IsDigit(text[end]))
                ++end;
    }

    if (text.substr(end, 2) == "hh" || text.substr(end, 2) == "ll")
        end += 2;
    else if (text.substr(end, 3) == "I32" || text.substr(end, 3) == "I64")
        end += 3;
    else if (end < text.size() && std::string_view("hljztLIw").find(text[end]) != std::string_view::npos)
        ++end;

    if (end >= text.size() || std::string_view("diouxXfFeEgGaAcCsSp").find(text[end]) == std::string_view::npos)
        return false;

    cursor = end + 1;
    directive = text.substr(begin, cursor - begin);
    return true;
}

bool HasMatchingFormat(std::string_view source, std::string_view translation)
{
    size_t sourceCursor = 0;
    size_t translationCursor = 0;
    for (;;)
    {
        std::string_view sourceDirective;
        std::string_view translationDirective;
        if (!NextDirective(source, sourceCursor, sourceDirective) ||
            !NextDirective(translation, translationCursor, translationDirective) || sourceDirective != translationDirective)
            return false;
        if (sourceDirective.empty())
            return true;
    }
}

bool LoadFile(const std::filesystem::path& path, Dictionary& dictionary)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    dictionary.chinese = true;
    bool firstLine = true;
    bool inTranslations = false;
    std::string line;
    while (std::getline(file, line))
    {
        std::string_view text(line);
        if (firstLine && text.starts_with("\xEF\xBB\xBF"))
            text.remove_prefix(3);
        firstLine = false;
        text = Trim(text);
        if (text.empty() || text.front() == '#' || text.front() == ';')
            continue;
        if (text.front() == '[')
        {
            const auto close = text.find(']');
            inTranslations = false;
            if (close != std::string_view::npos)
            {
                const auto remainder = Trim(text.substr(close + 1));
                inTranslations = Trim(text.substr(1, close - 1)) == "Translations" &&
                                 (remainder.empty() || remainder.front() == '#' || remainder.front() == ';');
            }
            continue;
        }
        if (!inTranslations)
            continue;

        size_t separator = 0;
        for (; separator < text.size(); ++separator)
        {
            if (text[separator] == '\\' && separator + 1 < text.size())
                ++separator;
            else if (text[separator] == '=')
                break;
        }
        if (separator == text.size())
            continue;

        auto key = Unescape(Trim(text.substr(0, separator)));
        auto value = Unescape(Trim(text.substr(separator + 1)));
        if (!IsUtf8(key) || !IsUtf8(value))
            continue;

        // Hidden labels are IDs, not prose. For visible labels the dictionary owns only the
        // visible part: never let a translated suffix rename or alias an ImGui widget.
        const auto suffix = key.find("##");
        if (suffix == 0)
            continue;
        const auto translatedSuffix = value.find("##");
        if (suffix != std::string::npos)
        {
            if (translatedSuffix != std::string::npos)
                value.erase(translatedSuffix);
            // A trailing '#' would turn an original ## suffix into ### when concatenated.
            if (value.empty() || value.back() == '#')
                continue;
            value.append(key, suffix, std::string::npos);
        }
        else if (translatedSuffix != std::string::npos)
            continue;

        if (HasMatchingFormat(key, value))
            dictionary.translations.insert_or_assign(std::move(key), std::move(value));
    }
    return true;
}

Dictionary LoadDictionary()
{
    Dictionary dictionary;
    try
    {
        const auto dllDirectory = Util::DllPath().parent_path();
        const auto gameDirectory = Util::ExePath().parent_path();
        for (const auto* directory : { &dllDirectory, &gameDirectory })
        {
            if (directory->empty())
                continue;
            if (LoadFile(*directory / L"OptiScaler_zh.ini", dictionary) ||
                LoadFile(*directory / L"zh_CN.ini", dictionary))
                return dictionary;
            if (dllDirectory == gameDirectory)
                break;
        }
    }
    catch (...)
    {
        // An optional language patch must never prevent the menu from opening.
        // Successfully read entries are still safe to publish, and missing entries fall back.
    }
    return dictionary;
}

const Dictionary& GetDictionary()
{
    // This is never reloaded or cleared by MenuCommon::Shutdown. ImGui contexts may be reset
    // while callers retain pointers to translated labels. Function-local initialization is
    // thread-safe; all lookups after publication are read-only and allocation-free.
    static const Dictionary dictionary = LoadDictionary();
    return dictionary;
}
} // namespace

void Localization::Init()
{
    (void) GetDictionary();
}

bool Localization::IsChinese()
{
    return GetDictionary().chinese;
}

const char* Tr(const char* text)
{
    if (text == nullptr)
        return text;
    const auto& translations = GetDictionary().translations;
    if (translations.empty())
        return text;
    const auto found = translations.find(std::string_view(text));
    return found == translations.end() ? text : found->second.c_str();
}
