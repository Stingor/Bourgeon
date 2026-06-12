from enum import IntEnum

from bourgeon import ui, log, register_callback


class MoonlightWindow:
    class UiMessage(IntEnum):
        WindowClosed = 1
        AlootTypeSelected = 2
        DiscordChecked = 3
        TextInputEdited = 4
        ListBoxItemSelected = 5
        ResetAlootTypePressed = 6

    def __init__(self):
        self.aloottype = ui.Combo("Alootype", ["Healing", "Usable", "Etc", "Armors", "Weapons", "Card", "PetEgg", "PetArmor", "Ammo"], self.UiMessage.AlootTypeSelected)
        self.resetaloottype = ui.Button("Reset", self.UiMessage.ResetAlootTypePressed)
        self.discord_chat_state = False
        self.discord_chat = ui.CheckBox("Discord chat Gonryun", self.discord_chat_state, self.UiMessage.DiscordChecked)
        self.text_input = ui.TextInput(
            "TextInput",
            "Hello",
            ui.AllowedChars.All,
            256,
            self.UiMessage.TextInputEdited,
        )
        self.list_box = ui.ListBox("ListBox", ["Line1", "Line2", "Line3"], self.UiMessage.ListBoxItemSelected)
        self.list_box.set_size((70, 60))
        self.window = ui.Window(
            "Moonlight-Destiny",
            [[ui.Text("Settings")], [ui.Separator()], [self.aloottype, self.resetaloottype],
             [self.discord_chat], [self.text_input], [self.list_box]],
            self.UiMessage.WindowClosed)

    def open(self) -> None:
        ui.register_window(self.window)

    def close(self) -> None:
        ui.unregister_window(self.window)

    def handle_messages(self) -> None:
        message = self.window.read()
        while message is not None:
            msg_id, values = message
            if msg_id == self.UiMessage.WindowClosed:
                self.close()
            elif msg_id == self.UiMessage.AlootTypeSelected:
                log(f"ALootType value selected: {values[0]}")
            elif msg_id == self.UiMessage.ResetAlootTypePressed:
                log(f"ALootType Reset has been pressed: {values[0]}")
            elif msg_id == self.UiMessage.DiscordChecked:
                self.discord_chat_state = values[0]
                log(f"CheckBox state: {self.discord_chat_state}")
            elif msg_id == self.UiMessage.TextInputEdited:
                log(f"TextInput has been edited: {values[0]}")
            elif msg_id == self.UiMessage.ListBoxItemSelected:
                log(f"ListBox item selected: {values[0]}")

            message = self.window.read()


moonlight_window = MoonlightWindow()
moonlight_window.open()
log("Moonlight-Destiny UI loaded!")


def on_tick() -> None:
    """
    OnTick callback.
    """
    global moonlight_window
    moonlight_window.handle_messages()


register_callback("OnTick", on_tick)