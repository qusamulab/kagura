/* Windows ETW integration points. Provider enumeration is unimplemented.
 * Both entry points return 0 for ABI compatibility.
 */
#include "../internal.h"

#ifdef _WIN32

int kagura_etw_provider_present(const wchar_t *provider_guid) {
    (void)provider_guid;
    return 0;
}

int kagura_etw_analysis_tool_check(void) {
    return 0;
}

#endif
