#include "vm_host.h"

vm_host &skein_vm_host() {
    static vm_host host;
    return host;
}
