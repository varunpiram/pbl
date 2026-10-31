#pragma once
#define PBL_STATE "/Library/Application Support/ProductivityBlocker"
#define PBL_BACKEND_BUNDLE "/Library/PrivilegedHelperTools/pbl Blocking Service.app"
// clang-format off: installer scripts parse these one-line string definitions.
#define PBL_WORKER "/Library/PrivilegedHelperTools/pbl Blocking Service.app/Contents/MacOS/local.productivityblocker.worker"
#define PBL_HELPER "/Library/PrivilegedHelperTools/pbl Blocking Service.app/Contents/MacOS/local.pbl.helper"
// clang-format on
#define PBL_SERVICE "local.pbl.helper"
#define PBL_HELPER_REQUIREMENT_FILE "/Library/PrivilegedHelperTools/local.pbl.helper.requirement"
