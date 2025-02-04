// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2023-2024, Advanced Micro Devices, Inc. All rights reserved.

#ifndef _BO_IPU_H_
#define _BO_IPU_H_

#include "../bo.h"
#include "drm_local/amdxdna_accel.h"

namespace shim_xdna {

class bo_ipu : public bo {
public:
  // Support BO creation from coreutil
  bo_ipu(const device& device, size_t size, uint64_t flags);
  // Support BO creation from internal
  bo_ipu(const device& device, size_t size, amdxdna_bo_type type);

  ~bo_ipu();

  void
  sync(direction dir, size_t size, size_t offset) override;

private:
  bo_ipu(const device& device, size_t size, uint64_t flags, amdxdna_bo_type type);
  const device& m_device;
};

} // namespace shim_xdna

#endif // _BO_IPU_H_
