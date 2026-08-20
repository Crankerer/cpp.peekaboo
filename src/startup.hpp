#pragma once

// Whether Windows launches PeekaBoo at sign-in. This is the per-user Run key,
// so it needs no elevation and touches nothing outside HKCU.
namespace pb::startup {

[[nodiscard]] bool enabled();

// Writes the path of the running executable, or removes the entry again.
// Returns false only if the registry refused us.
bool setEnabled(bool on);

}  // namespace pb::startup
