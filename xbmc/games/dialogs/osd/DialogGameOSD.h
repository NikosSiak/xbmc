/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "guilib/GUIDialog.h"

namespace KODI
{
namespace GAME
{
  class CDialogGameOSDHelp;

  class CDialogGameOSD : public CGUIDialog
  {
  public:
    CDialogGameOSD();

    ~CDialogGameOSD() override = default;

    // Implementation of CGUIControl via CGUIDialog
    bool OnMessage(CGUIMessage& message) override;

  protected:
    // Implementation of CGUIWindow via CGUIDialog
    void OnInitWindow() override;

  private:
    std::unique_ptr<CDialogGameOSDHelp> m_helpDialog;
 };
}
}
