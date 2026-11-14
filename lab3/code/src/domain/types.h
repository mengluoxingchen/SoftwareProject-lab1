#pragma once

#include <QtGlobal>

namespace ledger {

// A role is scoped to a ledger membership, never globally to a user.
enum class MemberRole { Administrator, Member, ReadOnly };
enum class MemberStatus { Active, Exited };
enum class LedgerStatus { Active, Archived, Deleted };
enum class SplitMethod { Equal, Ratio, FixedAmount };
enum class ExpenseStatus { Draft, PendingReview, Approved, Rejected, Withdrawn, Settled };

// Money is represented in the smallest currency unit. No binary floating-point amounts.
using AmountInCents = qint64;

}  // namespace ledger
