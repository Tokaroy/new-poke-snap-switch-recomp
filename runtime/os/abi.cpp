// Link-time half of the runtime ABI check (runtime/cpu.h SNAP_RUNTIME_ABI). The lifted function tables refer to snap_runtime_abi_<n>.
#include "cpu.h"

#define SNAP_ABI_CAT2(a, b) a##b
#define SNAP_ABI_CAT(a, b) SNAP_ABI_CAT2(a, b)
extern "C" {
extern const int SNAP_ABI_CAT(snap_runtime_abi_, SNAP_RUNTIME_ABI);
const int SNAP_ABI_CAT(snap_runtime_abi_, SNAP_RUNTIME_ABI) = SNAP_RUNTIME_ABI;
}
