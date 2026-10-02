// SPDX-License-Identifier: MIT OR GPL-3.0-only

use std::collections::{HashMap, HashSet};

use crossterm::event::{KeyCode, KeyEvent, KeyEventKind, KeyModifiers, ModifierKeyCode};

const SHIFT: u8 = 1 << 0;
const CONTROL: u8 = 1 << 1;
const ALT: u8 = 1 << 2;
const SUPER: u8 = 1 << 3;

const MODIFIERS: [(u8, u32); 4] = [(SHIFT, 50), (CONTROL, 37), (ALT, 64), (SUPER, 133)];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct KeyTransition {
    pub keycode: u32,
    pub pressed: bool,
}

#[derive(Default)]
pub struct KeyboardState {
    down: HashSet<u32>,
    physical_modifiers: HashMap<u8, HashSet<u32>>,
    synthetic_modifiers: HashMap<u8, u32>,
    pressed_keys: HashMap<u32, u8>,
}

impl KeyboardState {
    pub fn handle(&mut self, event: KeyEvent) -> Vec<KeyTransition> {
        if let KeyCode::Modifier(modifier) = event.code {
            return self.handle_modifier(modifier, event.kind);
        }

        let Some((keycode, implied_modifiers)) = translate_key(event.code) else {
            return Vec::new();
        };
        let desired = modifier_mask(event.modifiers) | implied_modifiers;
        let mut transitions = Vec::new();
        self.synchronize_modifiers(desired, &mut transitions);
        match event.kind {
            KeyEventKind::Press => {
                self.pressed_keys.insert(keycode, desired);
                self.emit(keycode, true, &mut transitions);
            }
            KeyEventKind::Repeat => {
                self.pressed_keys.insert(keycode, desired);
                // TinyX applies the server's repeat controls to a repeated press.
                transitions.push(KeyTransition {
                    keycode,
                    pressed: true,
                });
            }
            KeyEventKind::Release => {
                self.emit(keycode, false, &mut transitions);
                self.pressed_keys.remove(&keycode);
                let retained = self
                    .pressed_keys
                    .values()
                    .fold(0, |modifiers, value| modifiers | *value);
                self.synchronize_modifiers(retained, &mut transitions);
            }
        }
        transitions
    }

    pub fn clear(&mut self) {
        self.down.clear();
        self.physical_modifiers.clear();
        self.synthetic_modifiers.clear();
        self.pressed_keys.clear();
    }

    fn handle_modifier(
        &mut self,
        modifier: ModifierKeyCode,
        kind: KeyEventKind,
    ) -> Vec<KeyTransition> {
        let Some((class, keycode)) = modifier_key(modifier) else {
            return Vec::new();
        };
        let mut transitions = Vec::new();
        match kind {
            KeyEventKind::Press => {
                self.physical_modifiers
                    .entry(class)
                    .or_default()
                    .insert(keycode);
                self.emit(keycode, true, &mut transitions);
                if let Some(synthetic) = self.synthetic_modifiers.remove(&class) {
                    if synthetic != keycode {
                        self.emit(synthetic, false, &mut transitions);
                    }
                }
            }
            KeyEventKind::Repeat => {}
            KeyEventKind::Release => {
                self.emit(keycode, false, &mut transitions);
                if let Some(keys) = self.physical_modifiers.get_mut(&class) {
                    keys.remove(&keycode);
                    if keys.is_empty() {
                        self.physical_modifiers.remove(&class);
                    }
                }
            }
        }
        transitions
    }

    fn synchronize_modifiers(&mut self, desired: u8, transitions: &mut Vec<KeyTransition>) {
        // Release in reverse chord order before installing a new modifier set.
        for (class, _) in MODIFIERS.iter().rev() {
            let physically_down = self
                .physical_modifiers
                .get(class)
                .is_some_and(|keys| !keys.is_empty());
            if desired & class == 0 || physically_down {
                if let Some(keycode) = self.synthetic_modifiers.remove(class) {
                    self.emit(keycode, false, transitions);
                }
            }
        }
        for (class, keycode) in MODIFIERS {
            let physically_down = self
                .physical_modifiers
                .get(&class)
                .is_some_and(|keys| !keys.is_empty());
            if desired & class != 0
                && !physically_down
                && !self.synthetic_modifiers.contains_key(&class)
            {
                self.emit(keycode, true, transitions);
                self.synthetic_modifiers.insert(class, keycode);
            }
        }
    }

    fn emit(&mut self, keycode: u32, pressed: bool, transitions: &mut Vec<KeyTransition>) {
        if pressed {
            if self.down.insert(keycode) {
                transitions.push(KeyTransition { keycode, pressed });
            }
        } else if self.down.remove(&keycode) {
            transitions.push(KeyTransition { keycode, pressed });
        }
    }
}

fn modifier_mask(modifiers: KeyModifiers) -> u8 {
    let mut mask = 0;
    if modifiers.contains(KeyModifiers::SHIFT) {
        mask |= SHIFT;
    }
    if modifiers.contains(KeyModifiers::CONTROL) {
        mask |= CONTROL;
    }
    if modifiers.intersects(KeyModifiers::ALT | KeyModifiers::META) {
        mask |= ALT;
    }
    if modifiers.intersects(KeyModifiers::SUPER | KeyModifiers::HYPER) {
        mask |= SUPER;
    }
    mask
}

fn modifier_key(modifier: ModifierKeyCode) -> Option<(u8, u32)> {
    Some(match modifier {
        ModifierKeyCode::LeftShift => (SHIFT, 50),
        ModifierKeyCode::RightShift => (SHIFT, 62),
        ModifierKeyCode::LeftControl => (CONTROL, 37),
        ModifierKeyCode::RightControl => (CONTROL, 105),
        ModifierKeyCode::LeftAlt | ModifierKeyCode::LeftMeta => (ALT, 64),
        ModifierKeyCode::RightAlt
        | ModifierKeyCode::RightMeta
        | ModifierKeyCode::IsoLevel3Shift
        | ModifierKeyCode::IsoLevel5Shift => (ALT, 108),
        ModifierKeyCode::LeftSuper | ModifierKeyCode::LeftHyper => (SUPER, 133),
        ModifierKeyCode::RightSuper | ModifierKeyCode::RightHyper => (SUPER, 134),
    })
}

fn translate_key(code: KeyCode) -> Option<(u32, u8)> {
    Some(match code {
        KeyCode::Esc => (9, 0),
        KeyCode::Backspace => (22, 0),
        KeyCode::Tab => (23, 0),
        KeyCode::BackTab => (23, SHIFT),
        KeyCode::Enter => (36, 0),
        KeyCode::CapsLock => (66, 0),
        KeyCode::NumLock => (77, 0),
        KeyCode::ScrollLock => (78, 0),
        KeyCode::Home => (110, 0),
        KeyCode::Up => (111, 0),
        KeyCode::PageUp => (112, 0),
        KeyCode::Left => (113, 0),
        KeyCode::Right => (114, 0),
        KeyCode::End => (115, 0),
        KeyCode::Down => (116, 0),
        KeyCode::PageDown => (117, 0),
        KeyCode::Insert => (118, 0),
        KeyCode::Delete => (119, 0),
        KeyCode::Menu => (135, 0),
        KeyCode::F(number @ 1..=10) => (66 + u32::from(number), 0),
        KeyCode::F(11) => (95, 0),
        KeyCode::F(12) => (96, 0),
        KeyCode::Char(character) => translate_character(character)?,
        _ => return None,
    })
}

fn translate_character(character: char) -> Option<(u32, u8)> {
    let value = match character {
        '1' => (10, 0),
        '!' => (10, SHIFT),
        '2' => (11, 0),
        '@' => (11, SHIFT),
        '3' => (12, 0),
        '#' => (12, SHIFT),
        '4' => (13, 0),
        '$' => (13, SHIFT),
        '5' => (14, 0),
        '%' => (14, SHIFT),
        '6' => (15, 0),
        '^' => (15, SHIFT),
        '7' => (16, 0),
        '&' => (16, SHIFT),
        '8' => (17, 0),
        '*' => (17, SHIFT),
        '9' => (18, 0),
        '(' => (18, SHIFT),
        '0' => (19, 0),
        ')' => (19, SHIFT),
        '-' => (20, 0),
        '_' => (20, SHIFT),
        '=' => (21, 0),
        '+' => (21, SHIFT),
        '[' => (34, 0),
        '{' => (34, SHIFT),
        ']' => (35, 0),
        '}' => (35, SHIFT),
        ';' => (47, 0),
        ':' => (47, SHIFT),
        '\'' => (48, 0),
        '"' => (48, SHIFT),
        '`' => (49, 0),
        '~' => (49, SHIFT),
        '\\' => (51, 0),
        '|' => (51, SHIFT),
        ',' => (59, 0),
        '<' => (59, SHIFT),
        '.' => (60, 0),
        '>' => (60, SHIFT),
        '/' => (61, 0),
        '?' => (61, SHIFT),
        ' ' => (65, 0),
        value if value.is_ascii_alphabetic() => {
            let lower = value.to_ascii_lowercase();
            let keycode = match lower {
                'q' => 24,
                'w' => 25,
                'e' => 26,
                'r' => 27,
                't' => 28,
                'y' => 29,
                'u' => 30,
                'i' => 31,
                'o' => 32,
                'p' => 33,
                'a' => 38,
                's' => 39,
                'd' => 40,
                'f' => 41,
                'g' => 42,
                'h' => 43,
                'j' => 44,
                'k' => 45,
                'l' => 46,
                'z' => 52,
                'x' => 53,
                'c' => 54,
                'v' => 55,
                'b' => 56,
                'n' => 57,
                'm' => 58,
                _ => unreachable!(),
            };
            (keycode, u8::from(value.is_ascii_uppercase()) * SHIFT)
        }
        _ => return None,
    };
    Some(value)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn event(code: KeyCode, modifiers: KeyModifiers, kind: KeyEventKind) -> KeyEvent {
        KeyEvent::new_with_kind(code, modifiers, kind)
    }

    #[test]
    fn uppercase_keys_are_bracketed_by_synthetic_shift() {
        let mut keyboard = KeyboardState::default();
        assert_eq!(
            keyboard.handle(event(
                KeyCode::Char('A'),
                KeyModifiers::SHIFT,
                KeyEventKind::Press
            )),
            [
                KeyTransition {
                    keycode: 50,
                    pressed: true
                },
                KeyTransition {
                    keycode: 38,
                    pressed: true
                }
            ]
        );
        assert_eq!(
            keyboard.handle(event(
                KeyCode::Char('A'),
                KeyModifiers::SHIFT,
                KeyEventKind::Release
            )),
            [
                KeyTransition {
                    keycode: 38,
                    pressed: false
                },
                KeyTransition {
                    keycode: 50,
                    pressed: false
                }
            ]
        );
    }

    #[test]
    fn explicit_modifier_events_do_not_duplicate_synthetic_keys() {
        let mut keyboard = KeyboardState::default();
        assert_eq!(
            keyboard.handle(event(
                KeyCode::Modifier(ModifierKeyCode::LeftShift),
                KeyModifiers::SHIFT,
                KeyEventKind::Press
            )),
            [KeyTransition {
                keycode: 50,
                pressed: true
            }]
        );
        assert_eq!(
            keyboard.handle(event(
                KeyCode::Char('A'),
                KeyModifiers::SHIFT,
                KeyEventKind::Press
            )),
            [KeyTransition {
                keycode: 38,
                pressed: true
            }]
        );
        assert_eq!(
            keyboard.handle(event(
                KeyCode::Char('A'),
                KeyModifiers::SHIFT,
                KeyEventKind::Release
            )),
            [KeyTransition {
                keycode: 38,
                pressed: false
            }]
        );
        assert_eq!(
            keyboard.handle(event(
                KeyCode::Modifier(ModifierKeyCode::LeftShift),
                KeyModifiers::NONE,
                KeyEventKind::Release
            )),
            [KeyTransition {
                keycode: 50,
                pressed: false
            }]
        );
    }

    #[test]
    fn ctrl_alt_delete_has_ordered_modifier_transitions() {
        let mut keyboard = KeyboardState::default();
        let modifiers = KeyModifiers::CONTROL | KeyModifiers::ALT;
        assert_eq!(
            keyboard.handle(event(KeyCode::Delete, modifiers, KeyEventKind::Press)),
            [
                KeyTransition {
                    keycode: 37,
                    pressed: true
                },
                KeyTransition {
                    keycode: 64,
                    pressed: true
                },
                KeyTransition {
                    keycode: 119,
                    pressed: true
                }
            ]
        );
        assert_eq!(
            keyboard.handle(event(KeyCode::Delete, modifiers, KeyEventKind::Release)),
            [
                KeyTransition {
                    keycode: 119,
                    pressed: false
                },
                KeyTransition {
                    keycode: 64,
                    pressed: false
                },
                KeyTransition {
                    keycode: 37,
                    pressed: false
                }
            ]
        );
    }

    #[test]
    fn shifted_punctuation_implies_shift_even_if_terminal_omits_it() {
        let mut keyboard = KeyboardState::default();
        assert_eq!(
            keyboard.handle(event(
                KeyCode::Char('?'),
                KeyModifiers::NONE,
                KeyEventKind::Press
            )),
            [
                KeyTransition {
                    keycode: 50,
                    pressed: true
                },
                KeyTransition {
                    keycode: 61,
                    pressed: true
                }
            ]
        );
    }
}
