#include "DeleteScopePolicy.h"

namespace file_delete_policy {

Action resolve(const bool serverDeleteOffered, const int selectedIndex) {
  if (selectedIndex <= 0) return Action::Cancelled;
  if (selectedIndex == 1) return Action::DeleteLocalOnly;
  if (selectedIndex == 2 && serverDeleteOffered) return Action::DeleteLocalAndServer;
  // An index past what was actually offered (e.g. a stale 2 from a 2-way
  // dialog) is treated as Cancelled rather than guessed at -- silently
  // deleting less, or more, than the dialog the user actually saw promised
  // would be worse than doing nothing.
  return Action::Cancelled;
}

}  // namespace file_delete_policy
