/*
 *      Copyright (C) 2012-2016 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this Program; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "GameClient.h"
#include "GameClientCallbacks.h"
#include "GameClientInput.h"
#include "GameClientTranslator.h"
#include "addons/AddonManager.h"
#include "cores/AudioEngine/Utils/AEChannelInfo.h"
#include "dialogs/GUIDialogOK.h"
#include "FileItem.h"
#include "filesystem/Directory.h"
#include "filesystem/SpecialProtocol.h"
#include "games/addons/playback/GameClientBasicPlayback.h"
#include "games/addons/playback/GameClientReversiblePlayback.h"
#include "games/controllers/Controller.h"
#include "games/ports/PortManager.h"
#include "games/GameManager.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/WindowIDs.h"
#include "input/InputManager.h"
#include "input/joysticks/DefaultJoystick.h" // for DEFAULT_CONTROLLER_ID
#include "input/joysticks/JoystickTypes.h"
#include "peripherals/Peripherals.h"
#include "profiles/ProfilesManager.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "URL.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"

#include <algorithm>
#include <cstring>
#include <iterator>

using namespace GAME;

#define EXTENSION_SEPARATOR          "|"
#define EXTENSION_WILDCARD           "*"
#define BUTTON_INDEX_MASK            0x01ff

#define GAME_PROPERTY_EXTENSIONS           "extensions"
#define GAME_PROPERTY_SUPPORTS_GAME_LOOP   "supports_game_loop"
#define GAME_PROPERTY_SUPPORTS_VFS         "supports_vfs"
#define GAME_PROPERTY_SUPPORTS_STANDALONE  "supports_standalone"
#define GAME_PROPERTY_SUPPORTS_KEYBOARD    "supports_keyboard"

// --- NormalizeExtension ------------------------------------------------------

namespace
{
  /*
   * \brief Convert to lower case and canonicalize with a leading "."
   */
  std::string NormalizeExtension(const std::string& strExtension)
  {
    std::string ext = strExtension;

    if (!ext.empty() && ext != EXTENSION_WILDCARD)
    {
      StringUtils::ToLower(ext);

      if (ext[0] != '.')
        ext.insert(0, ".");
    }

    return ext;
  }
}

// --- CGameClient -------------------------------------------------------------

std::unique_ptr<CGameClient> CGameClient::FromExtension(ADDON::AddonProps props, const cp_extension_t* ext)
{
  using namespace ADDON;

  static const std::vector<std::string> properties = {
      GAME_PROPERTY_EXTENSIONS,
      GAME_PROPERTY_SUPPORTS_GAME_LOOP,
      GAME_PROPERTY_SUPPORTS_VFS,
      GAME_PROPERTY_SUPPORTS_STANDALONE,
      GAME_PROPERTY_SUPPORTS_KEYBOARD,
  };

  for (const auto& property : properties)
  {
    std::string strProperty = CAddonMgr::GetInstance().GetExtValue(ext->configuration, property.c_str());
    if (!strProperty.empty())
      props.extrainfo[property] = strProperty;
  }

  return std::unique_ptr<CGameClient>(new CGameClient(std::move(props)));
}

CGameClient::CGameClient(ADDON::AddonProps props) :
  CAddonDll<DllGameClient, GameClient, game_client_properties>(std::move(props)),
  m_apiVersion("0.0.0"),
  m_libraryProps(this, m_pInfo),
  m_bSupportsVFS(false),
  m_bSupportsGameLoop(false),
  m_bSupportsStandalone(false),
  m_bSupportsKeyboard(false),
  m_bIsPlaying(false),
  m_serializeSize(0),
  m_audio(nullptr),
  m_video(nullptr),
  m_region(GAME_REGION_UNKNOWN)
{
  const ADDON::InfoMap& extraInfo = m_props.extrainfo;
  ADDON::InfoMap::const_iterator it;

  it = extraInfo.find(GAME_PROPERTY_EXTENSIONS);
  if (it != extraInfo.end())
  {
    std::vector<std::string> extensions = StringUtils::Split(it->second, EXTENSION_SEPARATOR);
    std::transform(extensions.begin(), extensions.end(),
      std::inserter(m_extensions, m_extensions.begin()), NormalizeExtension);
  }

  it = extraInfo.find(GAME_PROPERTY_SUPPORTS_GAME_LOOP);
  if (it != extraInfo.end())
  {
    m_bSupportsGameLoop = (it->second == "true" || it->second == "yes");
  }
  else
  {
    // Empty value defaults to true
    m_bSupportsGameLoop = true;
  }

  it = extraInfo.find(GAME_PROPERTY_SUPPORTS_VFS);
  if (it != extraInfo.end())
    m_bSupportsVFS = (it->second == "true" || it->second == "yes");

  it = extraInfo.find(GAME_PROPERTY_SUPPORTS_STANDALONE);
  if (it != extraInfo.end())
    m_bSupportsStandalone = (it->second == "true" || it->second == "yes");

  it = extraInfo.find(GAME_PROPERTY_SUPPORTS_KEYBOARD);
  if (it != extraInfo.end())
    m_bSupportsKeyboard = (it->second == "true" || it->second == "yes");

  ResetPlayback();
}

CGameClient::~CGameClient(void)
{
}

bool CGameClient::IsType(ADDON::TYPE type) const
{
  return type == ADDON::ADDON_GAMEDLL ||
         type == ADDON::ADDON_GAME;
}

std::string CGameClient::LibPath() const
{
  // If the game client requires a proxy, load its DLL instead
  if (m_pInfo->proxy_dll_count > 0)
    return m_pInfo->proxy_dll_paths[0];

  return CAddon::LibPath();
}

void CGameClient::OnEnabled()
{
  //CGameManager::GetInstance().Enable(ID()); // TODO
}

void CGameClient::OnDisabled()
{
  //CGameManager::GetInstance().Disable(ID()); // TODO
}

ADDON::AddonPtr CGameClient::GetRunningInstance() const
{
  GameClientPtr gameAddon;
  if (CGameManager::GetInstance().GetAddonInstance(ID(), gameAddon))
    return std::static_pointer_cast<CAddon>(gameAddon);

  return CAddon::GetRunningInstance();
}

bool CGameClient::IsExtensionValid(const std::string& strExtension) const
{
  if (strExtension.empty())
    return false;

  if (m_extensions.empty())
    return false; // Game client didn't provide any extensions

  if (m_extensions.find(EXTENSION_WILDCARD) != m_extensions.end())
    return true; // Game client accepts all extensions

  return m_extensions.find(NormalizeExtension(strExtension)) != m_extensions.end();
}

bool CGameClient::CanOpen(const CFileItem& file) const
{
  // Try to resolve path to a local file, as not all game clients support VFS
  CURL translatedUrl(CSpecialProtocol::TranslatePath(file.GetPath()));
  if (translatedUrl.GetProtocol() == "file")
    translatedUrl.SetProtocol("");

  // Filter by vfs support
  const bool bIsLocalFS = translatedUrl.GetProtocol().empty();
  if (!bIsLocalFS && !m_bSupportsVFS)
    return false;

  // Directories not currently supported
  if (file.m_bIsFolder)
    return false;

  // Filter by extension
  if (!IsExtensionValid(URIUtils::GetExtension(translatedUrl.Get())))
    return false;

  return true;
}

bool CGameClient::Initialize(void)
{
  using namespace XFILE;

  // Ensure user profile directory exists for add-on
  if (!CDirectory::Exists(Profile()))
    CDirectory::Create(Profile());

  // Ensure directory exists for savestates
  std::string savestatesDir = URIUtils::AddFileToFolder(CProfilesManager::GetInstance().GetSavestatesFolder(), ID());
  if (!CDirectory::Exists(savestatesDir))
    CDirectory::Create(savestatesDir);

  m_libraryProps.InitializeProperties();

  if (Create() == ADDON_STATUS_OK)
  {
    LogAddonProperties();
    return true;
  }

  return false;
}

void CGameClient::Unload()
{
  Destroy();
}

bool CGameClient::OpenFile(const CFileItem& file, IGameAudioCallback* audio, IGameVideoCallback* video)
{
  if (audio == nullptr || video == nullptr)
    return false;

  CSingleLock lock(m_critSection);

  if (!Initialized())
    return false;

  CloseFile();

  bool bSuccess = false;

  if (CanOpen(file))
  {
    CLog::Log(LOGDEBUG, "GameClient: Loading %s", file.GetPath().c_str());

    try { bSuccess = LogError(m_pStruct->LoadGame(file.GetPath().c_str()), "LoadGame()"); }
    catch (...) { LogException("LoadGame()"); }
  }
  else if (m_bSupportsStandalone)
  {
    CLog::Log(LOGDEBUG, "GameClient: Loading %s in standalone mode", ID().c_str());

    try { bSuccess = LogError(m_pStruct->LoadStandalone(), "LoadStandalone()"); }
    catch (...) { LogException("LoadStandalone()"); }
  }

  // If gameplay failed, check for missing optional resources
  if (!bSuccess)
    NotifyMissingDep();

  if (bSuccess && LoadGameInfo(file.GetPath()) && NormalizeAudio(audio))
  {
    m_bIsPlaying      = true;
    m_gamePath        = file.GetPath();
    m_serializeSize   = GetSerializeSize();
    m_audio           = audio;
    m_video           = video;
    m_inputRateHandle = PERIPHERALS::g_peripherals.SetEventScanRate(m_timing.GetFrameRate()); // TODO: Convert event scanner to double

    if (m_bSupportsKeyboard)
      OpenKeyboard();

    // Start playback
    CreatePlayback();

    return true;
  }

  return false;
}

bool CGameClient::NormalizeAudio(IGameAudioCallback* audioCallback)
{
  unsigned int originalSampleRate = m_timing.GetSampleRate();

  if (m_timing.NormalizeAudio(audioCallback))
  {
    const bool bChanged = (originalSampleRate != m_timing.GetSampleRate());
    if (bChanged)
    {
      CLog::Log(LOGDEBUG, "GAME: Correcting audio and video by %f to avoid resampling", m_timing.GetCorrectionFactor());
      CLog::Log(LOGDEBUG, "GAME: Audio sample rate normalized to %u", m_timing.GetSampleRate());
      CLog::Log(LOGDEBUG, "GAME: Video frame rate scaled to %f", m_timing.GetFrameRate());
    }
    else
    {
      CLog::Log(LOGDEBUG, "GAME: Audio sample rate is supported, no scaling or resampling needed");
    }
  }
  else
  {
    CLog::Log(LOGERROR, "GAME: Failed to normalize audio sample rate: exceeds %u%% difference", CGameClientTiming::MAX_CORRECTION_FACTOR_PERCENT);
    return false;
  }

  return true;
}

bool CGameClient::LoadGameInfo(const std::string& logPath)
{
  // Get information about system audio/video timings and geometry
  // Can be called only after retro_load_game()
  game_system_av_info av_info = { };

  bool bSuccess = false;
  try { bSuccess = LogError(m_pStruct->GetGameInfo(&av_info), "GetGameInfo()"); }
  catch (...) { LogException("GetGameInfo()"); }

  if (!bSuccess)
    return false;

  GAME_REGION region;
  try { region = m_pStruct->GetRegion(); }
  catch (...) { LogException("GetRegion()"); return false; }

  CLog::Log(LOGINFO, "GAME: ---------------------------------------");
  CLog::Log(LOGINFO, "GAME: Opened file %s",   logPath.c_str());
  CLog::Log(LOGINFO, "GAME: Base Width:   %u", av_info.geometry.base_width);
  CLog::Log(LOGINFO, "GAME: Base Height:  %u", av_info.geometry.base_height);
  CLog::Log(LOGINFO, "GAME: Max Width:    %u", av_info.geometry.max_width);
  CLog::Log(LOGINFO, "GAME: Max Height:   %u", av_info.geometry.max_height);
  CLog::Log(LOGINFO, "GAME: Aspect Ratio: %f", av_info.geometry.aspect_ratio);
  CLog::Log(LOGINFO, "GAME: FPS:          %f", av_info.timing.fps);
  CLog::Log(LOGINFO, "GAME: Sample Rate:  %f", av_info.timing.sample_rate);
  CLog::Log(LOGINFO, "GAME: Region:       %s", CGameClientTranslator::TranslateRegion(region));
  CLog::Log(LOGINFO, "GAME: ---------------------------------------");

  m_timing.SetFrameRate(av_info.timing.fps);
  m_timing.SetSampleRate(av_info.timing.sample_rate);
  m_region = region;

  return true;
}

void CGameClient::NotifyMissingDep()
{
  using namespace ADDON;

  const ADDONDEPS& dependencies = GetDeps();
  for (ADDONDEPS::const_iterator it = dependencies.begin(); it != dependencies.end(); ++it)
  {
    const bool bOptional = it->second.second;
    if (bOptional)
    {
      const std::string& strAddonId = it->first;
      AddonPtr dummy;
      const bool bInstalled = CAddonMgr::GetInstance().GetAddon(strAddonId, dummy);
      if (!bInstalled)
      {
        // Failed to play game
        // The add-on %s is missing.
        CGUIDialogOK::ShowAndGetInput(g_localizeStrings.Get(35209), StringUtils::Format(g_localizeStrings.Get(35210).c_str(), strAddonId.c_str()));
        break;
      }
    }
  }
}

void CGameClient::CreatePlayback()
{
  if (m_bSupportsGameLoop)
  {
    m_playback.reset(new CGameClientReversiblePlayback(this, m_timing.GetFrameRate(), m_serializeSize));
  }
  else
  {
    ResetPlayback();
  }
}

void CGameClient::ResetPlayback()
{
  m_playback.reset(new CGameClientDummyPlayback);
}

void CGameClient::Reset()
{
  ResetPlayback();

  CSingleLock lock(m_critSection);

  if (m_bIsPlaying)
  {
    try { LogError(m_pStruct->Reset(), "Reset()"); }
    catch (...) { LogException("Reset()"); }

    CreatePlayback();
  }
}

void CGameClient::CloseFile()
{
  ResetPlayback();

  CSingleLock lock(m_critSection);

  if (m_bIsPlaying)
  {
    try { LogError(m_pStruct->UnloadGame(), "UnloadGame()"); }
    catch (...) { LogException("UnloadGame()"); }
  }

  ClearPorts();

  if (m_bSupportsKeyboard)
    CloseKeyboard();

  m_bIsPlaying = false;
  m_gamePath.clear();
  m_serializeSize = 0;
  if (m_inputRateHandle)
  {
    m_inputRateHandle->Release();
    m_inputRateHandle.reset();
  }

  m_audio = nullptr;
  m_video = nullptr;
  m_timing.Reset();
}

void CGameClient::RunFrame()
{
  CSingleLock lock(m_critSection);

  if (m_bIsPlaying)
  {
    try { LogError(m_pStruct->RunFrame(), "RunFrame()"); }
    catch (...) { LogException("RunFrame()"); }
  }
}

bool CGameClient::OpenPixelStream(GAME_PIXEL_FORMAT format, unsigned int width, unsigned int height)
{
  if (!m_video)
    return false;

  AVPixelFormat pixelFormat = CGameClientTranslator::TranslatePixelFormat(format);
  if (pixelFormat == AV_PIX_FMT_NONE)
  {
    CLog::Log(LOGERROR, "GAME: Unknown pixel format: %d", format);
    return false;
  }

  return m_video->OpenPixelStream(pixelFormat, width, height, m_timing.GetFrameRate());
}

bool CGameClient::OpenVideoStream(GAME_VIDEO_CODEC codec)
{
  if (!m_video)
    return false;

  AVCodecID videoCodec = CGameClientTranslator::TranslateVideoCodec(codec);
  if (videoCodec == AV_CODEC_ID_NONE)
  {
    CLog::Log(LOGERROR, "GAME: Unknown video format: %d", codec);
    return false;
  }

  return m_video->OpenEncodedStream(videoCodec);
}

bool CGameClient::OpenPCMStream(GAME_PCM_FORMAT format, const GAME_AUDIO_CHANNEL* channelMap)
{
  if (!m_audio || channelMap == nullptr)
    return false;

  AEDataFormat pcmFormat = CGameClientTranslator::TranslatePCMFormat(format);
  if (pcmFormat == AE_FMT_INVALID)
  {
    CLog::Log(LOGERROR, "GAME: Unknown PCM format: %d", format);
    return false;
  }

  CAEChannelInfo channelLayout;
  for (const GAME_AUDIO_CHANNEL* channelPtr = channelMap; *channelPtr != GAME_CH_NULL; channelPtr++)
  {
    AEChannel channel = CGameClientTranslator::TranslateAudioChannel(*channelPtr);
    if (channel == AE_CH_NULL)
    {
      CLog::Log(LOGERROR, "GAME: Unknown channel ID: %d", *channelPtr);
      return false;
    }
    channelLayout += channel;
  }

  return m_audio->OpenPCMStream(pcmFormat, m_timing.GetSampleRate(), channelLayout);
}

bool CGameClient::OpenAudioStream(GAME_AUDIO_CODEC codec, const GAME_AUDIO_CHANNEL* channelMap)
{
  if (!m_audio)
    return false;

  AVCodecID audioCodec = CGameClientTranslator::TranslateAudioCodec(codec);
  if (audioCodec == AV_CODEC_ID_NONE)
  {
    CLog::Log(LOGERROR, "GAME: Unknown audio codec: %d", codec);
    return false;
  }

  CAEChannelInfo channelLayout;
  for (const GAME_AUDIO_CHANNEL* channelPtr = channelMap; *channelPtr != GAME_CH_NULL; channelPtr++)
  {
    AEChannel channel = CGameClientTranslator::TranslateAudioChannel(*channelPtr);
    if (channel == AE_CH_NULL)
    {
      CLog::Log(LOGERROR, "GAME: Unknown channel ID: %d", *channelPtr);
      return false;
    }
    channelLayout += channel;
  }

  return m_audio->OpenEncodedStream(audioCodec, m_timing.GetSampleRate(), channelLayout);
}

void CGameClient::AddStreamData(GAME_STREAM_TYPE stream, const uint8_t* data, unsigned int size)
{
  switch (stream)
  {
  case GAME_STREAM_AUDIO:
  {
    if (m_audio)
      m_audio->AddData(data, size);
    break;
  }
  case GAME_STREAM_VIDEO:
  {
    if (m_video)
      m_video->AddData(data, size);
    break;
  }
  default:
    break;
  }
}

void CGameClient::CloseStream(GAME_STREAM_TYPE stream)
{
  switch (stream)
  {
  case GAME_STREAM_AUDIO:
  {
    if (m_video)
      m_video->CloseStream();
    break;
  }
  case GAME_STREAM_VIDEO:
  {
    if (m_audio)
      m_audio->CloseStream();
    break;
  }
  default:
    break;
  }
}

size_t CGameClient::GetSerializeSize()
{
  CSingleLock lock(m_critSection);

  size_t serializeSize = 0;
  if (m_bIsPlaying)
  {
    try { serializeSize = m_pStruct->SerializeSize(); }
    catch (...) { LogException("SerializeSize()"); }
  }

  return serializeSize;
}

bool CGameClient::Serialize(uint8_t* data, size_t size)
{
  if (data == nullptr || size == 0)
    return false;

  CSingleLock lock(m_critSection);

  bool bSuccess = false;
  if (m_bIsPlaying)
  {
    try { bSuccess = LogError(m_pStruct->Serialize(data, size), "Serialize()"); }
    catch (...) { LogException("Serialize()"); }
  }

  return bSuccess;
}

bool CGameClient::Deserialize(const uint8_t* data, size_t size)
{
  if (data == nullptr || size == 0)
    return false;

  CSingleLock lock(m_critSection);

  bool bSuccess = false;
  if (m_bIsPlaying)
  {
    try { bSuccess = LogError(m_pStruct->Deserialize(data, size), "Deserialize()"); }
    catch (...) { LogException("Deserialize()"); }
  }

  return bSuccess;
}

bool CGameClient::OpenPort(unsigned int port)
{
  // Fail if port is already open
  if (port < m_ports.size() && m_ports[port] != nullptr)
    return false;

  ControllerVector controllers = GetControllers();
  if (!controllers.empty())
  {
    // TODO: Choose controller
    ControllerPtr& controller = controllers[0];

    if (controller->LoadLayout())
    {
      ClosePort(port);

      // Ensure port exists
      if (port >= m_ports.size())
        m_ports.resize(port + 1);

      m_ports[port] = new CGameClientInput(this, port, controller);

      // If keyboard input is being captured by this add-on, force the port type to PERIPHERAL_JOYSTICK
      PERIPHERALS::PeripheralType device = PERIPHERALS::PERIPHERAL_UNKNOWN;
      if (m_bSupportsKeyboard)
        device = PERIPHERALS::PERIPHERAL_JOYSTICK;

      CPortManager::GetInstance().OpenPort(m_ports[port], port, device);

      UpdatePort(port, controller);

      return true;
    }
  }

  return false;
}

void CGameClient::ClosePort(unsigned int port)
{
  // Can't close port if it doesn't exist
  if (port >= m_ports.size())
    return;

  if (m_ports[port] != nullptr)
  {
    CPortManager::GetInstance().ClosePort(m_ports[port]);

    delete m_ports[port];
    m_ports[port] = nullptr;

    UpdatePort(port, CController::EmptyPtr);
  }
}

void CGameClient::UpdatePort(unsigned int port, const ControllerPtr& controller)
{
  using namespace JOYSTICK;

  if (controller != CController::EmptyPtr)
  {
    std::string strId = controller->ID();

    game_controller controllerStruct;

    controllerStruct.controller_id        = strId.c_str();
    controllerStruct.digital_button_count = controller->Layout().FeatureCount(FEATURE_TYPE::SCALAR, INPUT_TYPE::DIGITAL);
    controllerStruct.analog_button_count  = controller->Layout().FeatureCount(FEATURE_TYPE::SCALAR, INPUT_TYPE::ANALOG);
    controllerStruct.analog_stick_count   = controller->Layout().FeatureCount(FEATURE_TYPE::ANALOG_STICK);
    controllerStruct.accelerometer_count  = controller->Layout().FeatureCount(FEATURE_TYPE::ACCELEROMETER);
    controllerStruct.key_count            = 0; // TODO
    controllerStruct.rel_pointer_count    = 0; // TODO
    controllerStruct.abs_pointer_count    = 0; // TODO
    controllerStruct.motor_count          = controller->Layout().FeatureCount(FEATURE_TYPE::MOTOR);

    try { m_pStruct->UpdatePort(port, true, &controllerStruct); }
    catch (...) { LogException("UpdatePort()"); }
  }
  else
  {
    try { m_pStruct->UpdatePort(port, false, nullptr); }
    catch (...) { LogException("UpdatePort()"); }
  }
}

bool CGameClient::HasFeature(const std::string& controller, const std::string& feature)
{
  try { return m_pStruct->HasFeature(controller.c_str(), feature.c_str()); }
  catch (...) { LogException("HasFeature()"); }

  return false;
}

void CGameClient::ClearPorts(void)
{
  for (unsigned int i = 0; i < m_ports.size(); i++)
    ClosePort(i);
}

ControllerVector CGameClient::GetControllers(void) const
{
  using namespace ADDON;

  ControllerVector controllers;

  const ADDONDEPS& dependencies = GetDeps();
  for (ADDONDEPS::const_iterator it = dependencies.begin(); it != dependencies.end(); ++it)
  {
    AddonPtr addon;
    if (CAddonMgr::GetInstance().GetAddon(it->first, addon, ADDON_GAME_CONTROLLER))
    {
      ControllerPtr controller = std::dynamic_pointer_cast<CController>(addon);
      if (controller)
        controllers.push_back(controller);
    }
  }

  if (controllers.empty())
  {
    // Use the default controller
    AddonPtr addon;
    if (CAddonMgr::GetInstance().GetAddon(DEFAULT_CONTROLLER_ID, addon, ADDON_GAME_CONTROLLER))
      controllers.push_back(std::static_pointer_cast<CController>(addon));
  }

  return controllers;
}

bool CGameClient::OnButtonPress(int port, const std::string& feature, bool bPressed)
{
  // Only allow activated input in fullscreen game
  if (bPressed && !AcceptsInput())
  {
    CLog::Log(LOGDEBUG, "GameClient: button press ignored, not in fullscreen game");
    return false;
  }

  std::string strControllerId = m_ports[port]->ControllerID();

  bool bHandled = false;

  game_input_event event;

  event.type                   = GAME_INPUT_EVENT_DIGITAL_BUTTON;
  event.port                   = port;
  event.controller_id          = strControllerId.c_str();
  event.feature_name           = feature.c_str();
  event.digital_button.pressed = bPressed;

  try { bHandled = m_pStruct->InputEvent(&event); }
  catch (...) { LogException("InputEvent()"); }

  return bHandled;
}

bool CGameClient::OnButtonMotion(int port, const std::string& feature, float magnitude)
{
  // Only allow activated input in fullscreen game
  if (magnitude && !AcceptsInput())
    return false;

  std::string strControllerId = m_ports[port]->ControllerID();

  bool bHandled = false;

  game_input_event event;

  event.type                    = GAME_INPUT_EVENT_ANALOG_BUTTON;
  event.port                    = port;
  event.controller_id           = strControllerId.c_str();
  event.feature_name            = feature.c_str();
  event.analog_button.magnitude = magnitude;

  try { bHandled = m_pStruct->InputEvent(&event); }
  catch (...) { LogException("InputEvent()"); }

  return bHandled;
}

bool CGameClient::OnAnalogStickMotion(int port, const std::string& feature, float x, float y)
{
  // Only allow activated input in fullscreen game
  if ((x || y) && !AcceptsInput())
    return false;

  std::string strControllerId = m_ports[port]->ControllerID();

  bool bHandled = false;

  game_input_event event;

  event.type           = GAME_INPUT_EVENT_ANALOG_STICK;
  event.port           = port;
  event.controller_id  = strControllerId.c_str();
  event.feature_name   = feature.c_str();
  event.analog_stick.x = x;
  event.analog_stick.y = y;

  try { bHandled = m_pStruct->InputEvent(&event); }
  catch (...) { LogException("InputEvent()"); }

  return bHandled;
}

bool CGameClient::OnAccelerometerMotion(int port, const std::string& feature, float x, float y, float z)
{
  // Only allow activated input in fullscreen game
  if ((x || y || z) && !AcceptsInput())
    return false;

  std::string strControllerId = m_ports[port]->ControllerID();

  bool bHandled = false;

  game_input_event event;

  event.type            = GAME_INPUT_EVENT_ACCELEROMETER;
  event.port            = port;
  event.controller_id   = strControllerId.c_str();
  event.feature_name    = feature.c_str();
  event.accelerometer.x = x;
  event.accelerometer.y = y;
  event.accelerometer.z = z;

  try { bHandled = m_pStruct->InputEvent(&event); }
  catch (...) { LogException("InputEvent()"); }

  return bHandled;
}

bool CGameClient::ReceiveInputEvent(const game_input_event& event)
{
  bool bHandled = false;

  switch (event.type)
  {
  case GAME_INPUT_EVENT_MOTOR:
    if (event.feature_name)
      bHandled = SetRumble(event.port, event.feature_name, event.motor.magnitude);
    break;
  default:
    break;
  }

  return bHandled;
}

bool CGameClient::SetRumble(unsigned int port, const std::string& feature, float magnitude)
{
  bool bHandled = false;

  if (port < m_ports.size() && m_ports[port] != nullptr)
    bHandled = m_ports[port]->SetRumble(feature, magnitude);

  return bHandled;
}

void CGameClient::OpenKeyboard(void)
{
  CInputManager::GetInstance().RegisterKeyboardHandler(this);
}

void CGameClient::CloseKeyboard(void)
{
  CInputManager::GetInstance().UnregisterKeyboardHandler(this);
}


bool CGameClient::OnKeyPress(const CKey& key)
{
  // Only allow activated input in fullscreen game
  if (!AcceptsInput())
  {
    CLog::Log(LOGDEBUG, "GameClient: key press ignored, not in fullscreen game");
    return false;
  }

  bool bHandled = false;

  game_input_event event;

  event.type            = GAME_INPUT_EVENT_KEY;
  event.port            = 0;
  event.controller_id   = ""; // TODO
  event.feature_name    = ""; // TODO
  event.key.pressed     = true;
  event.key.character   = key.GetButtonCode() & BUTTON_INDEX_MASK;
  event.key.modifiers   = CGameClientTranslator::GetModifiers(static_cast<CKey::Modifier>(key.GetModifiers()));

  if (event.key.character != 0)
  {
    try { bHandled = m_pStruct->InputEvent(&event); }
    catch (...) { LogException("InputEvent()"); }
  }

  return bHandled;
}

void CGameClient::OnKeyRelease(const CKey& key)
{
  game_input_event event;

  event.type            = GAME_INPUT_EVENT_KEY;
  event.port            = 0;
  event.controller_id   = ""; // TODO
  event.feature_name    = ""; // TODO
  event.key.pressed     = false;
  event.key.character   = key.GetButtonCode() & BUTTON_INDEX_MASK;
  event.key.modifiers   = CGameClientTranslator::GetModifiers(static_cast<CKey::Modifier>(key.GetModifiers()));

  if (event.key.character != 0)
  {
    try { m_pStruct->InputEvent(&event); }
    catch (...) { LogException("InputEvent()"); }
  }
}

bool CGameClient::AcceptsInput(void)
{
  return g_windowManager.GetActiveWindowID() == WINDOW_FULLSCREEN_GAME;
}

void CGameClient::LogAddonProperties(void) const
{
  std::vector<std::string> vecExtensions(m_extensions.begin(), m_extensions.end());

  CLog::Log(LOGINFO, "GAME: ------------------------------------");
  CLog::Log(LOGINFO, "GAME: Loaded DLL for %s", ID().c_str());
  CLog::Log(LOGINFO, "GAME: Client: %s at version %s", Name().c_str(), Version().asString().c_str());
  CLog::Log(LOGINFO, "GAME: Valid extensions: %s", StringUtils::Join(vecExtensions, " ").c_str());
  CLog::Log(LOGINFO, "GAME: Supports VFS:                  %s", m_bSupportsVFS ? "yes" : "no");
  CLog::Log(LOGINFO, "GAME: Supports game loop:            %s", m_bSupportsGameLoop ? "yes" : "no");
  CLog::Log(LOGINFO, "GAME: Supports standalone execution: %s", m_bSupportsStandalone ? "yes" : "no");
  CLog::Log(LOGINFO, "GAME: Supports keyboard:             %s", m_bSupportsKeyboard ? "yes" : "no");
  CLog::Log(LOGINFO, "GAME: ------------------------------------");
}

bool CGameClient::LogError(GAME_ERROR error, const char* strMethod) const
{
  if (error != GAME_ERROR_NO_ERROR)
  {
    CLog::Log(LOGERROR, "GAME - %s - addon '%s' returned an error: %s",
        strMethod, ID().c_str(), CGameClientTranslator::ToString(error));
    return false;
  }
  return true;
}

void CGameClient::LogException(const char* strFunctionName) const
{
  CLog::Log(LOGERROR, "GAME: exception caught while trying to call '%s' on add-on %s",
      strFunctionName, ID().c_str());
  CLog::Log(LOGERROR, "Please contact the developer of this add-on: %s", Author().c_str());
}
