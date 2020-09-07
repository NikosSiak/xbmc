/*
 *  Copyright (C) 2020 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "view/GUIViewControl.h"
#include "guilib/GUIDialog.h"

namespace KODI
{
namespace GAME
{
class CGameServices;

class CDialogRetroAchievementsLogin : public CGUIDialog
{
public:
  CDialogRetroAchievementsLogin();
  bool OnMessage(CGUIMessage& message) override;

private:
  CGUIViewControl m_viewControl;
  CGameServices& m_gameServices;
};
} // namespace GAME
} // namespace KODI
