#pragma once

#include <string>

// One-shot GitHub release check. The FIRST call to Start() (the first time the
// netplay menu is entered) spawns a background worker that fetches the latest
// release tag once per process; nothing is ever re-polled. Every other call is
// a no-op. The menu layer polls the cheap accessors below to decorate OPTIONS /
// About with a "[!]" badge while a newer release exists that the user has not
// yet looked at (opening About acknowledges the CURRENT latest tag; the badge
// then stays hidden until an even newer tag is published).
namespace netplay::update_check
{
// Idempotent. Gated by the CheckForUpdates setting. Never blocks: the HTTPS
// round trip runs on its own thread and publishes atomically.
void Start();

// Latest release tag known so far ("" until the fetch completes / on failure).
std::string LatestVersion();

// True once the fetch has completed and the latest tag is strictly newer than
// this build - regardless of acknowledgement (the About panel uses this).
bool HasNewerRelease();

// HasNewerRelease() && the user has not opened About since that tag appeared.
// Drives the "[!]" badge.
bool IsUpdateAvailable();

// Badge text for label builders: "[!]" when IsUpdateAvailable(), else "".
const char* BadgeText();

// Records the current latest tag as seen (persisted next to the DLL) so the
// badge disappears until a newer release is published. No-op when there is
// nothing newer to acknowledge.
void AcknowledgeLatest();

// Public page for the user to fetch the release from.
const char* ReleasesPageUrl();

// Opens ReleasesPageUrl() in the user's default browser (ShellExecute "open").
// Returns false when the shell refused; never blocks on the browser.
bool OpenReleasesPage();
} // namespace netplay::update_check
