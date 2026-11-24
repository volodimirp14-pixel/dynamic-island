#pragma once
// Donation links shown in the Launcher ("Поддержать проект").
// Paste the real addresses here; while a link is empty its button says
// "Ссылка скоро появится" and does nothing. Only https:// links are opened.
namespace di::links {
inline constexpr const wchar_t* kDonateEu = L"https://donatello.to/DynamicIsland";  // Donatello: Europe and the rest of the world
inline constexpr const wchar_t* kDonateRu = L"https://donatepay.ru/don/1545186";  // DonatePay: Russia and CIS
}  // namespace di::links
