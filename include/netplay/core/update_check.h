#pragma once

#include <string>

namespace netplay::update_check
{
void Start();
std::string LatestVersion();
bool HasNewerRelease();
bool IsUpdateAvailable();
const char* BadgeText();
void AcknowledgeLatest();
const char* ReleasesPageUrl();
bool OpenReleasesPage();
} // namespace netplay::update_check
