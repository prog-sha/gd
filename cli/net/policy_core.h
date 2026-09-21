// Evaluate certificate policy mappings through a shared-node graph rather than an expanding tree.
#pragma once
#include "cert_store.h"

namespace GDCrypto {
bool certificate_policies(const std::vector<Cert::Ptr> &path, const std::vector<Bytes> &requested = {}); // Check a leaf-to-anchor path's policy constraints after its signatures and authority are verified.
}
