#include <fuse/Fuse.h>
#include <fuse/HookInjection.h>



#include "platform/Platform.h"
//
#include <ddraw.h>
#include <detours.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <filesystem>

#include "GameProxy.h"
#include "Bot.h"
#include "Debug.h"
#include "Time.h"
#include "KeyController.h"

using namespace fuse;


#define UM_SETTEXT WM_USER + 0x69

const std::string kEnabledText = "Continuum (enabled) - ";
const std::string kDisabledText = "Continuum (disabled) - ";

using time_clock = std::chrono::high_resolution_clock;
using time_point = time_clock::time_point;
using seconds = std::chrono::duration<float>;

static time_point g_LastUpdateTime;
static time_point g_StartTime = time_clock::now();



static HRESULT(STDMETHODCALLTYPE* RealBlt)(LPDIRECTDRAWSURFACE, LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDBLTFX);

HRESULT STDMETHODCALLTYPE OverrideBlt(LPDIRECTDRAWSURFACE surface, LPRECT dest_rect, LPDIRECTDRAWSURFACE next_surface,
                                      LPRECT src_rect, DWORD flags, LPDDBLTFX fx) {
  u32 graphics_addr = *(u32*)(0x4C1AFC) + 0x30;
  LPDIRECTDRAWSURFACE primary_surface = (LPDIRECTDRAWSURFACE) * (u32*)(graphics_addr + 0x40);
  LPDIRECTDRAWSURFACE back_surface = (LPDIRECTDRAWSURFACE) * (u32*)(graphics_addr + 0x44);

  // Check if flipping. I guess there's a full screen blit instead of flip when running without vsync?
  if (surface == primary_surface && next_surface == back_surface && fx == 0) {
    marvin::g_RenderState.Render();
  }

  return RealBlt(surface, dest_rect, next_surface, src_rect, flags, fx);
}


class Marvin final : public HookInjection {
 public:
  const char* GetHookName() override { return "Marvin"; }

  Marvin() {}

  void OnUpdate() override { 
      bool in_game = Fuse::Get().IsInGame();
      if (!in_game) return;

      g_hWnd = Fuse::Get().GetGameWindowHandle();
      name = Fuse::Get().GetName();

      if (!bot) {
        BuildMarvin();
        enabled = true;
      }

      // Check for key presses to enable/disable the bot.
      if (GetFocus() == g_hWnd) {
        if (GetAsyncKeyState(VK_F10)) {
          enabled = false;
          SetWindowText(g_hWnd, (kDisabledText + name).c_str());
        } else if (GetAsyncKeyState(VK_F9)) {
          enabled = true;
          SetWindowText(g_hWnd, (kEnabledText + name).c_str());
        }
      }

      if (ProcessChat() || Fuse::Get().GetConnectState() == ConnectState::Disconnected) {
        // TODO: move this into the fuse api - exits game back into menu
        u8* leave_ptr = (u8*)(Fuse::Get().GetGameMemory().game_address + 0x127ec + 0x58c);
        *leave_ptr = 1;
      }
  }

  bool OnMenuUpdate(BOOL hasMsg, LPMSG lpMsg, HWND hWnd) override {
      // If we get a menu message pump update then we must be out of the game, so clean it up.
      if (enabled) {
        CleanupMarvin();
        enabled = false;
      }

      return false;
  }

  virtual KeyState OnGetAsyncKeyState(int vKey) {

      // Don't override the enable/disable keys.
      if (vKey >= VK_F9 && vKey <= VK_F10) {
        return {};
      }

#if DEBUG_USER_CONTROL
      if (1) {
#else
      if (!enabled) {
#endif

        if (GetFocus() == g_hWnd) {
          // We want to retain user control here so don't do anything.
          return {};
        }

        // Force nothing being pressed when we don't have focus and bot isn't controlling itself.
        return {true, false};
      } else if (bot && bot->GetKeys().IsPressed(vKey)) {
        // Force press the requested key since the bot says it should be pressed.
        return {true, true};
      }

      // We want this to be forced off since the bot has control but doesn't want to press the key.
      return {true, false};
  }

  bool OnPeekMessage(LPMSG lpMsg, HWND hWnd) override {
      time_point now = time_clock::now();
      seconds dt = now - g_LastUpdateTime;

      if (!enabled) return false;



      if (bot && dt.count() > (float)(1.0f / bot->GetUpdateInterval())) {
#if DEBUG_RENDER
        marvin::g_RenderState.renderable_texts.clear();
        marvin::g_RenderState.renderable_lines.clear();
#endif
        if (bot) {
          bot->Update(dt.count());
          UpdateCRC();
        }
        g_LastUpdateTime = now;
      }


        return false;
  }

 private:

  void BuildMarvin(); 
  void CleanupMarvin();
  void UpdateCRC();
  bool ProcessChat();

  bool enabled = true;
  HWND g_hWnd = 0;
  std::string name;
  std::unique_ptr<marvin::Bot> bot = nullptr;
};


// This function needs to be called whenever anything changes in Continuum's memory.
// Continuum keeps track of its memory by calculating a checksum over it. Any changes to the memory outside of the
// normal game update would be caught. So we bypass this by manually calculating the crc at the end of the bot update
// and replacing the expected crc in memory.
void Marvin::UpdateCRC() {
  typedef u32(__fastcall * CRCFunction)(void* This, void* thiscall_garbage);
  CRCFunction func_ptr = (CRCFunction)0x43BB80;

  u32 game_addr = (*(u32*)0x4c1afc) + 0x127ec;
  u32 result = func_ptr((void*)game_addr, 0);

  *(u32*)(game_addr + 0x6d4) = result;
}

bool Marvin::ProcessChat() {

  auto& game = bot->GetGame();

  std::string name = game.GetPlayer().name;
  std::string eg_msg = "[ " + name + " ]";
  std::string eg_packet_loss_msg = "Packet loss too high for you to enter the game.";
  std::string hs_lag_msg = "You are too lagged to play in this arena.";

  std::vector<marvin::ChatMessage> chat = bot->GetGame().GetCurrentChat();

  for (marvin::ChatMessage msg : chat) {

        bool eg_lag_locked =
            msg.message.compare(0, 4 + name.size(), eg_msg) == 0 && game.GetZone() == marvin::Zone::ExtremeGames;

        bool eg_locked_in_spec =
            eg_lag_locked || msg.message == eg_packet_loss_msg && game.GetZone() == marvin::Zone::ExtremeGames;

        bool hs_locked_in_spec = msg.message == hs_lag_msg && game.GetZone() == marvin::Zone::Hyperspace;

      if (msg.type == marvin::ChatType::Arena) {
        if (eg_locked_in_spec || hs_locked_in_spec) {
          return true;
        }
      }
  }

  return false;
}


/* there are limitations on what win32 calls/actions can be made inside of this funcion call (DLLMain) */
void Marvin::BuildMarvin() {
   
  marvin::PerformanceTimer timer; 
  
  // create pointer to game and pass the window handle
  auto game = std::make_unique<marvin::ContinuumGameProxy>(g_hWnd);
  bot = std::make_unique<marvin::Bot>(std::move(game));
 
 marvin::log.Write("INITIALIZE MARVIN - NEW TIMER", timer.GetElapsedTime());
     
#if DEBUG_RENDER

 u32 graphics_addr = *(u32*)(0x4C1AFC) + 0x30;
 LPDIRECTDRAWSURFACE surface = (LPDIRECTDRAWSURFACE) * (u32*)(graphics_addr + 0x44);
 void** vtable = (*(void***)surface);
 RealBlt = (HRESULT(STDMETHODCALLTYPE*)(LPDIRECTDRAWSURFACE surface, LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD,
                                        LPDDBLTFX))vtable[5];

  DetourRestoreAfterWith();
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)RealBlt, OverrideBlt);
  DetourTransactionCommit();

  marvin::log.Write("Detours attached.", timer.GetElapsedTime());
#endif


  

  SetWindowText(g_hWnd, (kEnabledText + name).c_str());


  marvin::log.Write("FINISH INITIALIZE MARVIN - TOTAL TIME", timer.TimeSinceConstruction());
  
}



void Marvin::CleanupMarvin() {

  marvin::log.Write("CLEANUP MARVIN.");

#if DEBUG_RENDER
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourDetach(&(PVOID&)RealBlt, OverrideBlt);
  DetourTransactionCommit();
#endif
 

  SetWindowText(g_hWnd, "Continuum");
  bot = nullptr;
  marvin::log.Write("CLEANUP MARVIN FINISHED.");

}



BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID reserved) {
  switch (dwReason) {
      case DLL_PROCESS_ATTACH: {
        // InitializeMarvin();
        Fuse::Get().RegisterHook(std::make_unique<Marvin>());
      } break;
  }
  return TRUE;
}
