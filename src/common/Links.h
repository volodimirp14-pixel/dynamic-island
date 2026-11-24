#pragma once
// Donation links shown in the Launcher ("Поддержать проект").
// Paste the real addresses here; while a link is empty its button says
// "Ссылка скоро появится" and does nothing. Only https:// links are opened.
namespace di::links {
inline constexpr const wchar_t* kDonateEu = L"";  // donation for the EU (cards, PayPal, ...)
inline constexpr const wchar_t* kDonateRu = L"";  // donation for Russia (Boosty, ...)
}  // namespace di::links
