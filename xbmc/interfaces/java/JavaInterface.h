/*
 *  Copyright (C) 2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "interfaces/generic/ILanguageInvocationHandler.h"

namespace JAVA
{
class CJavaInterface : public ILanguageInvocationHandler
{
public:
  ~CJavaInterface() override = default;

  // Implementation of ILanguageInvocationHandler
  ILanguageInvoker* CreateInvoker() override;
};
}