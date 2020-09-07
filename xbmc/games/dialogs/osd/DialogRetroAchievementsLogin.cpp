/*
 *  Copyright (C) 2020 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DialogRetroAchievementsLogin.h"
#include "games/GameServices.h"
#include "games/GameSettings.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIMessage.h"
#include "guilib/WindowIDs.h"
#include "guilib/GUIWindowManager.h"
#include "ServiceBroker.h"

using namespace KODI;
using namespace GAME;

#define CONTROL_USERNAME_INPUT 1
#define CONTROL_PASSWORD_INPUT 2
#define CONTROL_LOGIN_BUTTON   3

CDialogRetroAchievementsLogin::CDialogRetroAchievementsLogin()
  : CGUIDialog(WINDOW_DIALOG_RETRO_ACHIEVEMEMTS_LOGIN, "DialogRetroAchievementsLogin.xml"),
    m_gameServices(CServiceBroker::GetGameServices())
{
}

bool CDialogRetroAchievementsLogin::OnMessage(CGUIMessage& message)
{
    // CServiceBroker::GetGameServices()
  switch (message.GetMessage())
  {
    case GUI_MSG_WINDOW_INIT:
    {
      CGUIDialog::OnMessage(message);
      {
        CGUIMessage msg(GUI_MSG_LABEL_SET, GetID(), CONTROL_USERNAME_INPUT);
        msg.SetLabel(/*m_gameServices.GameSettings().RAUsername()*/"aaaaaa");
        CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage(msg, GetID());
      }

      {
        CGUIMessage msg(GUI_MSG_LABEL_SET, GetID(), CONTROL_PASSWORD_INPUT);
        msg.SetLabel(/*m_gameServices.GameSettings().RAPassword()*/"bbbbbb");
        CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage(msg, GetID());
      }
    }
    case GUI_MSG_CLICKED:
    {
      if (message.GetSenderId() == CONTROL_LOGIN_BUTTON)
      {
        int x = 0;
      }
    }
  }

  return CGUIDialog::OnMessage(message);
}
