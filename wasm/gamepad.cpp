#include "moonlight_wasm.hpp"

#include <iostream>
#include <array>
#include <utility>
#include <sstream>
#include <chrono>
#include <thread>
#include <cmath>

#include <Limelight.h>
#include <emscripten/emscripten.h>

// Define a combination of buttons on the Xbox controller to stop streaming
const short STOP_STREAM_BUTTONS_FLAGS = LB_FLAG | RB_FLAG | BACK_FLAG | PLAY_FLAG;
bool isMouseEmulationActive = false;

// For explanation on ordering, see: https://www.w3.org/TR/gamepad/#remapping
// Enumeration for gamepad axes
enum GamepadAxis {
  LeftX = 0,
  LeftY = 1,
  RightX = 2,
  RightY = 3,
};

// Enumeration for gamepad buttons
enum GamepadButton {
  A, B, X, Y,
  LeftShoulder, RightShoulder,
  LeftTrigger, RightTrigger,
  Back, Play,
  LeftStick, RightStick,
  Up, Down, Left, Right,
  Special,
  Count,
};

// Function to create a mask for active gamepads
static short GetActiveGamepadMask(int numGamepads) {
  short result = 0;
  for (int i = 0; i < numGamepads; ++i) {
    result |= (1 << i);
  }
  return result;
}

// Function to map gamepad buttons to flags
static short GetButtonFlags(const EmscriptenGamepadEvent& gamepad) {
  // Triggers are considered analog buttons in Emscripten API, however they
  // need to be passed in separate arguments for Limelight (it even lacks flags
  // for them).

  // Define button mappings
  static const int buttonMasks[] {
    A_FLAG, B_FLAG, X_FLAG, Y_FLAG,
    LB_FLAG, RB_FLAG,
    0 /* LEFT_TRIGGER */, 0 /* RIGHT_TRIGGER */,
    BACK_FLAG, PLAY_FLAG,
    LS_CLK_FLAG, RS_CLK_FLAG,
    UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG,
    SPECIAL_FLAG,
  };

  static const int buttonMasksSize = static_cast<int>(sizeof(buttonMasks) / sizeof(buttonMasks[0]));

  short result = 0;
  for (int i = 0; i < gamepad.numButtons && i < buttonMasksSize; ++i) {
    if (gamepad.digitalButton[i] == EM_TRUE) {
      result |= buttonMasks[i];
    }
  }
  return result;
}

// Function to poll gamepad input
void MoonlightInstance::PollGamepads() {
  if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS) {
    std::cerr << "Sample gamepad data failed!\n";
    return;
  }

  const auto numGamepads = emscripten_get_num_gamepads();
  if (numGamepads == EMSCRIPTEN_RESULT_NOT_SUPPORTED) {
    std::cerr << "Get num gamepads failed!\n";
    return;
  }

  // Create a mask for active gamepads
  const auto activeGamepadMask = GetActiveGamepadMask(numGamepads);

  // Iterate through connected gamepads and process their input
  for (int gamepadID = 0; gamepadID < numGamepads; ++gamepadID) {
    emscripten_sample_gamepad_data();
    EmscriptenGamepadEvent gamepad;
    const auto result = emscripten_get_gamepad_status(gamepadID, &gamepad);
    if (result != EMSCRIPTEN_RESULT_SUCCESS || !gamepad.connected) {
      continue; /* Skip disconnected gamepads */
    }

    const auto buttonFlags = GetButtonFlags(gamepad);
    const auto leftTrigger = gamepad.analogButton[GamepadButton::LeftTrigger]
      * std::numeric_limits<unsigned char>::max();
    const auto rightTrigger = gamepad.analogButton[GamepadButton::RightTrigger]
      * std::numeric_limits<unsigned char>::max();
    const auto leftStickX = gamepad.axis[GamepadAxis::LeftX]
      * std::numeric_limits<short>::max();
    const auto leftStickY = -gamepad.axis[GamepadAxis::LeftY]
      * std::numeric_limits<short>::max();
    const auto rightStickX = gamepad.axis[GamepadAxis::RightX]
      * std::numeric_limits<short>::max();
    const auto rightStickY = -gamepad.axis[GamepadAxis::RightY]
      * std::numeric_limits<short>::max();

    if (buttonFlags == STOP_STREAM_BUTTONS_FLAGS) {
      PostToJs(std::string("stopping stream, button flags is ") + std::to_string(buttonFlags));
      stopStream();
      return;
    }

    static auto startPressTime = std::chrono::steady_clock::now();

    if (buttonFlags & PLAY_FLAG) {
      auto currentTime = std::chrono::steady_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startPressTime).count();
      if (duration >= 1000) { // Adjust this value to change how long start needs to be pressed 
        if (!isMouseEmulationActive) {
          isMouseEmulationActive = true;
          PostToJs("mouseEmulation enabled");
        } else {
          isMouseEmulationActive = false;
          PostToJs("mouseEmulation disabled");
        }
        startPressTime = std::chrono::steady_clock::now();
      }
    } else {
      startPressTime = std::chrono::steady_clock::now();
    }

    // If mouse emulation is on, left stick values are used with LiSendMouseMoveEvent
    if (isMouseEmulationActive) {
        const float baseMouseSpeed = 10.0f;
        const float stickMagnitude = std::sqrt(leftStickX * leftStickX + leftStickY * leftStickY) / std::numeric_limits<short>::max();
        const float mouseSpeed = baseMouseSpeed * stickMagnitude; // Mouse speed increases when the stick is further from the center 

        const float mouseXDelta = static_cast<float>(leftStickX) / std::numeric_limits<short>::max() * mouseSpeed;
        const float mouseYDelta = -static_cast<float>(leftStickY) / std::numeric_limits<short>::max() * mouseSpeed;

        LiSendMouseMoveEvent(static_cast<int>(mouseXDelta), static_cast<int>(mouseYDelta));

        // Right Stick Y is used for scrolling
        const float baseScrollSpeed = 1.0f;
        const float rightStickMagnitude = std::sqrt(rightStickX * rightStickX + rightStickY * rightStickY) / std::numeric_limits<short>::max();
        const float scrollSpeed = baseScrollSpeed * rightStickMagnitude;
        const float scrollDelta = static_cast<float>(rightStickY) / std::numeric_limits<short>::max() * scrollSpeed;

        LiSendScrollEvent(static_cast<int>(scrollDelta));

        // Buttons are mapped to mouse clicks 

        if (buttonFlags & (A_FLAG | LB_FLAG)) {
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        } else {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        }

        if (buttonFlags & (B_FLAG | RB_FLAG)) {
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
        } else {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
        }

        if (buttonFlags & RS_CLK_FLAG) {
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MIDDLE);
        } else {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
        }

    } else { // Send gamepad input to the desired handler (act as a normal gamepad)
        LiSendMultiControllerEvent(
            gamepadID, activeGamepadMask, buttonFlags, leftTrigger,
            rightTrigger, leftStickX, leftStickY, rightStickX, rightStickY);
    }
  }
}

void MoonlightInstance::ClControllerRumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor) {
    const float weakMagnitude = static_cast<float>(highFreqMotor) / static_cast<float>(UINT16_MAX);
    const float strongMagnitude = static_cast<float>(lowFreqMotor) / static_cast<float>(UINT16_MAX);

    std::ostringstream ss;
    ss << controllerNumber << "," << weakMagnitude << "," << strongMagnitude;

    PostToJs(std::string("controllerRumble: ") + ss.str());
}
