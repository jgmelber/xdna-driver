// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2024, Advanced Micro Devices, Inc. All rights reserved.

#ifndef PCIDEV_IPU_H
#define PCIDEV_IPU_H

#include "../pcidev.h"


namespace shim_xdna {

class pdev_ipu : public pdev
{
public:
  pdev_ipu(std::shared_ptr<const drv> driver, std::string sysfs_name);
  ~pdev_ipu();
 
  std::shared_ptr<xrt_core::device>
  create_device(xrt_core::device::handle_type handle, xrt_core::device::id_type id) const override;

  void
  open() const override;
};

} // namespace shim_xdna

#endif
