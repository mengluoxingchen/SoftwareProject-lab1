#pragma once

#include <QString>

namespace ledger::PasswordPolicy {

inline constexpr int kMinimumLength = 8;
inline constexpr int kMaximumLength = 32;

// New-account policy shared by the registration form and the authoritative service.
// Login intentionally verifies the stored password without imposing a new policy.
inline QString registrationError(const QString& password) {
    if (password.size() < kMinimumLength || password.size() > kMaximumLength) {
        return QStringLiteral("密码长度应为8–32位。");
    }
    bool has_letter = false;
    bool has_digit = false;
    for (const QChar character : password) {
        const ushort value = character.unicode();
        if (value < 0x21 || value > 0x7e) {
            return QStringLiteral("密码仅支持英文字母、数字和英文符号，不能包含空格。");
        }
        has_letter |= (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        has_digit |= value >= '0' && value <= '9';
    }
    if (!has_letter || !has_digit) {
        return QStringLiteral("密码须同时包含英文字母和数字，可搭配英文符号。");
    }
    return {};
}

}  // namespace ledger::PasswordPolicy
