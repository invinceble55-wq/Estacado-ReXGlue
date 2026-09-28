/**
 * @file        ui/d3d12/streamline_signature.cpp
 * @brief       NVIDIA's own Streamline module signature check
 *
 * sl_security.h defines UNICODE and non-inline functions, so it is kept alone
 * in this unit.
 *
 * @license     BSD 3-Clause License
 */

#if REX_HAVE_STREAMLINE

#include <sl_security.h>

namespace rex::ui::d3d12::streamline {

// The module's OS-verified signature plus NVIDIA's secondary signature.
bool VerifyStreamlineSignature(const wchar_t* path) {
  return sl::security::verifyEmbeddedSignature(path);
}

}  // namespace rex::ui::d3d12::streamline

#endif  // REX_HAVE_STREAMLINE
