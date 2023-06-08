//==========================================================
// Version header
// Version information is set in preprocessor definitions
// Akseli Lukkarila
//==========================================================

#pragma once
namespace version
{
constexpr auto APP_NAME = BUILDTIME_APP_NAME;
constexpr auto BRANCH = BUILDTIME_BRANCH;
constexpr auto BUILD_NAME = BUILDTIME_BUILD_NAME;
constexpr auto COMMIT = BUILDTIME_COMMIT;
constexpr auto DATE = BUILDTIME_DATE;
constexpr auto VERSION_INFO = BUILDTIME_VERSION_INFO;
constexpr auto VERSION_NUMBER = BUILDTIME_VERSION_NUMBER;
}  // namespace version
