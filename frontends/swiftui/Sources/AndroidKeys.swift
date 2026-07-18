/*
 * AndroidKeys.swift — chuyển sự kiện bàn phím macOS (NSEvent) sang control message của core:
 * phím điều hướng/sửa → KEY (Android keycode), tổ hợp Ctrl/Alt+alnum → KEY kèm metastate, phím
 * thường → TEXT. Tái hiện input.c của front-end GTK.
 */
import AppKit
import RCCore

// Android keycode cho các phím đặc biệt (khớp android.view.KeyEvent).
private let AKEYCODE_ENTER: UInt32 = 66
private let AKEYCODE_DEL: UInt32 = 67          // backspace
private let AKEYCODE_FORWARD_DEL: UInt32 = 112
private let AKEYCODE_TAB: UInt32 = 61
private let AKEYCODE_DPAD_LEFT: UInt32 = 21
private let AKEYCODE_DPAD_RIGHT: UInt32 = 22
private let AKEYCODE_DPAD_UP: UInt32 = 19
private let AKEYCODE_DPAD_DOWN: UInt32 = 20
private let AKEYCODE_MOVE_HOME: UInt32 = 122
private let AKEYCODE_MOVE_END: UInt32 = 123
private let AKEYCODE_PAGE_UP: UInt32 = 92
private let AKEYCODE_PAGE_DOWN: UInt32 = 93

enum KeyAction {
    case key(UInt32, UInt32) // keycode, metastate
    case text(String)
    case ignore
}

/// NSEvent modifier → Android metastate (kèm biến thể *_LEFT vì nhiều app kiểm tra bit đó).
func androidMeta(_ flags: NSEvent.ModifierFlags) -> UInt32 {
    var meta: UInt32 = 0
    if flags.contains(.shift) { meta |= 0x1 | 0x40 }       // SHIFT_ON | SHIFT_LEFT_ON
    if flags.contains(.option) { meta |= 0x02 | 0x10 }     // ALT_ON | ALT_LEFT_ON
    if flags.contains(.control) { meta |= 0x1000 | 0x2000 } // CTRL_ON | CTRL_LEFT_ON
    return meta
}

/// Chữ/số → Android keycode (A=29..Z=54, 0=7..9=16, SPACE=62); nil nếu không phải.
/// Dùng cho tổ hợp Ctrl/Alt — phím thường vẫn đi đường TEXT.
private func alnumToAndroid(_ ch: Character) -> UInt32? {
    let lower = Character(ch.lowercased())
    if let a = lower.asciiValue {
        if a >= 0x61 && a <= 0x7a { return 29 + UInt32(a - 0x61) } // a-z
        if a >= 0x30 && a <= 0x39 { return 7 + UInt32(a - 0x30) }  // 0-9
        if a == 0x20 { return 62 }                                 // space
    }
    return nil
}

/// Phím điều hướng/sửa → Android keycode; nil nếu nên gửi dạng TEXT.
private func specialToAndroid(_ scalar: Unicode.Scalar) -> UInt32? {
    switch Int(scalar.value) {
    case 0x0D, 0x03: return AKEYCODE_ENTER          // Return, keypad Enter
    case 0x7F: return AKEYCODE_DEL                   // Backspace (mac "delete")
    case 0x09, 0x19: return AKEYCODE_TAB             // Tab, Shift-Tab
    case 0x1B: return UInt32(RC_AKEYCODE_BACK)       // Escape → BACK
    case Int(NSDeleteFunctionKey): return AKEYCODE_FORWARD_DEL
    case Int(NSLeftArrowFunctionKey): return AKEYCODE_DPAD_LEFT
    case Int(NSRightArrowFunctionKey): return AKEYCODE_DPAD_RIGHT
    case Int(NSUpArrowFunctionKey): return AKEYCODE_DPAD_UP
    case Int(NSDownArrowFunctionKey): return AKEYCODE_DPAD_DOWN
    case Int(NSHomeFunctionKey): return AKEYCODE_MOVE_HOME
    case Int(NSEndFunctionKey): return AKEYCODE_MOVE_END
    case Int(NSPageUpFunctionKey): return AKEYCODE_PAGE_UP
    case Int(NSPageDownFunctionKey): return AKEYCODE_PAGE_DOWN
    default: return nil
    }
}

/// Quyết định KEY / TEXT / bỏ qua cho một keyDown.
func mapKeyDown(_ event: NSEvent) -> KeyAction {
    let flags = event.modifierFlags
    let meta = androidMeta(flags)

    // charactersIgnoringModifiers cho phím đặc biệt (arrow, enter, esc...).
    if let raw = event.charactersIgnoringModifiers, let first = raw.unicodeScalars.first,
       let code = specialToAndroid(first) {
        return .key(code, meta)
    }

    // Tổ hợp Ctrl/Alt → KEY (để app nhận đúng shortcut) thay vì TEXT.
    if flags.contains(.control) || flags.contains(.option) {
        if let ch = event.charactersIgnoringModifiers?.first, let code = alnumToAndroid(ch) {
            return .key(code, meta)
        }
    }

    // Phím thường → TEXT (ký tự đã áp modifier, ví dụ Shift).
    if let chars = event.characters, !chars.isEmpty {
        // Bỏ ký tự điều khiển (< 0x20) và DEL.
        if let s = chars.unicodeScalars.first, s.value >= 0x20 && s.value != 0x7F {
            return .text(chars)
        }
    }
    return .ignore
}

/// Với keyUp: android keycode tương ứng (đặc biệt hoặc alnum) để gửi UP nếu đã DOWN; nil nếu không.
func mapKeyUpCode(_ event: NSEvent) -> UInt32? {
    if let raw = event.charactersIgnoringModifiers, let first = raw.unicodeScalars.first,
       let code = specialToAndroid(first) {
        return code
    }
    if let ch = event.charactersIgnoringModifiers?.first, let code = alnumToAndroid(ch) {
        return code
    }
    return nil
}
