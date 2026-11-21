#pragma once
// Pure keyboard state machine for Win+E interception. It tracks modifier
// down/up events itself instead of consulting GetAsyncKeyState, whose state can
// remain stale after a swallowed key-up.

#include <windows.h>

namespace ze::platform {

enum class WinEKeyAction {
    Pass,
    Swallow,
    Launch,
    MaskAndPass
};

struct WinEKeyState {
    bool lwin = false;
    bool rwin = false;
    bool lctrl = false;
    bool rctrl = false;
    bool lalt = false;
    bool ralt = false;
    bool eDown = false;
    bool comboUsed = false;

    void Reset() { *this = WinEKeyState{}; }

    bool WinDown() const { return lwin || rwin; }
    bool CtrlDown() const { return lctrl || rctrl; }
    bool AltDown() const { return lalt || ralt; }

    WinEKeyAction Handle(DWORD vk, bool down, bool up) {
        const bool winKey = vk == VK_LWIN || vk == VK_RWIN;

        // Explorer performs the Win-key action on key-up. Once Win+E was
        // consumed, swallow that final Win-up as well.
        if (up && winKey && comboUsed) {
            UpdateModifier(vk, false);
            comboUsed = false;
            return WinEKeyAction::MaskAndPass;
        }

        UpdateModifier(vk, down);

        if (vk == 'E' && down) {
            if (WinDown() && !CtrlDown() && !AltDown()) {
                const bool firstPress = !eDown;
                eDown = true;
                comboUsed = true;
                return firstPress ? WinEKeyAction::Launch
                                  : WinEKeyAction::Swallow;
            }
        } else if (vk == 'E' && up && eDown) {
            eDown = false;
            return WinEKeyAction::Swallow;
        }
        return WinEKeyAction::Pass;
    }

private:
    void UpdateModifier(DWORD vk, bool value) {
        switch (vk) {
        case VK_LWIN: lwin = value; break;
        case VK_RWIN: rwin = value; break;
        case VK_LCONTROL: lctrl = value; break;
        case VK_RCONTROL: rctrl = value; break;
        case VK_CONTROL:
            lctrl = value;
            rctrl = value;
            break;
        case VK_LMENU: lalt = value; break;
        case VK_RMENU: ralt = value; break;
        case VK_MENU:
            lalt = value;
            ralt = value;
            break;
        default:
            break;
        }
    }
};

} // namespace ze::platform
