import bourgeon
import ragnarok_client as client
from bourgeon import ui
from ragnarok_client import Mode


class BasicInfoWindow:
    def __init__(self, name: str) -> None:
        self._hp_text = ui.Text("--")
        self._sp_text = ui.Text("--")
        self.window = ui.Window(name, [[
            ui.Text("HP"),
            self._hp_text,
            ui.Text("| SP"),
            self._sp_text,
        ]], 0)

    def open(self) -> None:
        ui.register_window(self.window)

    def close(self) -> None:
        ui.unregister_window(self.window)

    def update(self, hp: int, max_hp: int, sp: int, max_sp: int) -> None:
        self._hp_text.set_text(f"{hp} / {max_hp}")
        self._sp_text.set_text(f"{sp} / {max_sp}")


basic_info_window = None


_tick_count = 0

def on_tick() -> None:
    global _tick_count
    _tick_count += 1
    if _tick_count % 20 == 0:  # every ~2 seconds
        hp, max_hp = client.get_hp(), client.get_max_hp()
        sp, max_sp = client.get_sp(), client.get_max_sp()
        if max_hp > 0:
            client.print_in_chat(f"HP: {hp}/{max_hp}  SP: {sp}/{max_sp}", 0x00FF00, 0)


def on_mode_switch(mode_type: Mode, _map_name: str) -> None:
    bourgeon.log(f"OnModeSwitch: {mode_type}")
    if mode_type == Mode.Game:
        client.print_in_chat("Bourgeon loaded!", 0x7289DA, 0)


bourgeon.register_callback("OnTick", on_tick)
bourgeon.register_callback("OnModeSwitch", on_mode_switch)
