#pragma once

namespace Localization
{
// Loaded once for the DLL lifetime, independently of ImGui context creation/destruction.
void Init();
bool IsChinese();
} // namespace Localization

// Exact, case-sensitive UTF-8 lookup. Returned dictionary strings remain valid until DLL unload;
// missing keys return text itself. Include the original ##/### suffix in label keys.
// Translation files must preserve every printf directive verbatim and in the original order.
const char* Tr(const char* text);
